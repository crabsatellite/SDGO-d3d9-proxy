"""Prepare and run an isolated normal-client SDGO GameTest.

The runner copies a real client, keeps its installed Mods and assets, overlays
the native observer Mod and, when staged, the current production match_ai DLL,
then starts the copied normal client against an already-running backend. It
never edits the source client or starts a server.
"""
from __future__ import annotations

import argparse
import configparser
import ctypes
from ctypes import wintypes
import datetime as dt
import hashlib
import json
import ipaddress
import os
from pathlib import Path
import re
import secrets
import shutil
import stat
import socket
import struct
import string
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
import importlib.util


SCHEMA_VERSION = 2
HOST_KEY = "GOnline-54BE1568-00C97000"
GAME_EXE = "GOnline.exe"
STARTUP_EVENT_NAME = "9YOU SDGO"
TEST_MOD = "game_test_client"
TARGET_MOD = "match_ai"
FLOW_CASES = (
    "framework.bootstrap",
    "flow.login",
    "flow.onboarding",
    "pvp.room",
    "pvp.start",
    "pvp.ai_spawn",
    "pvp.return",
)
SUPPORTED_PVP_MODES = (1, 3, 6, 7, 9)
MODE_FLOW_STEPS = ("room", "start", "ai_spawn", "return")
IGNORED_DIRECTORIES = {
    ".git",
    ".sdgo-updater",
    ".svn",
    "__pycache__",
    "app_log",
    "logs",
    "screenshots",
    "zoa_log",
}
IGNORED_SUFFIXES = {".bak", ".dmp", ".log", ".tmp"}
IMMUTABLE_ROOT_SUFFIXES = {".dll", ".exe", ".zdx", ".zpk"}
ALNUM = string.ascii_letters + string.digits


class GameTestError(RuntimeError):
    pass


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def atomic_write(path: Path, data: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(data, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def read_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise GameTestError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise GameTestError(f"{path} is not a JSON object")
    return value


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def copy_ignore(directory: str, names: list[str]) -> set[str]:
    ignored = set()
    parent = Path(directory).name.lower()
    for name in names:
        lowered = name.lower()
        if (
            lowered in IGNORED_DIRECTORIES
            or Path(lowered).suffix in IGNORED_SUFFIXES
            or "backup" in lowered
            or "备份" in name
            or (parent == "sdgo" and lowered == "gametest")
        ):
            ignored.add(name)
    return ignored


def git_source_ref(root: Path) -> dict:
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "HEAD"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout.strip()
        status = subprocess.run(
            ["git", "status", "--porcelain"],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
            timeout=5,
        ).stdout
        return {"commit": commit, "dirty": bool(status.strip())}
    except (OSError, subprocess.SubprocessError):
        return {"commit": None, "dirty": None}


def ini_parser() -> configparser.ConfigParser:
    parser = configparser.ConfigParser(interpolation=None)
    parser.optionxform = str
    return parser


def read_ini(path: Path) -> configparser.ConfigParser:
    parser = ini_parser()
    try:
        raw=path.read_bytes()
        if raw.startswith((b'\xff\xfe',b'\xfe\xff')):
            decoded=raw.decode('utf-16')
        else:
            try:decoded=raw.decode('utf-8-sig')
            except UnicodeDecodeError:decoded=raw.decode('cp936')
        parser.read_string(decoded)
    except (OSError, UnicodeError, configparser.Error) as error:
        raise GameTestError(f"invalid INI file {path}: {error}") from error
    return parser


def write_ini(path: Path, parser: configparser.ConfigParser) -> None:
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        parser.write(stream, space_around_delimiters=False)
    os.replace(temporary, path)


def copy_file_if_missing(source: Path, destination: Path) -> None:
    if destination.is_file():
        return
    if not source.is_file():
        raise GameTestError(f"normal client lacks {destination.name} and stage lacks fallback: {source}")
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)


def manifest_requires(path: Path) -> set[str]:
    try:
        parser = read_ini(path)
    except GameTestError:
        # An unrelated diagnostic may have legacy duplicate settings sections.
        # Dependencies belong to its first native [mod] section; do not rewrite
        # or reinterpret that diagnostic's authored settings.
        raw=path.read_bytes()
        decoded=raw.decode('utf-16' if raw.startswith((b'\xff\xfe',b'\xfe\xff')) else 'utf-8-sig')
        match=re.search(r'(?ims)^\s*\[mod\]\s*\n(.*?)(?=^\s*\[|\Z)',decoded)
        if not match:raise GameTestError(f'manifest lacks [mod]: {path}') from None
        parser=ini_parser();parser.read_string('[mod]\n'+match[1])
    try:
        value = parser["mod"].get("requires", "")
    except KeyError as error:
        raise GameTestError(f"manifest lacks [mod]: {path}") from error
    return {item.strip() for item in value.split(",") if item.strip()}


def merge_required_symbols(stage_symbols: Path, copied_symbols: Path, manifests: list[Path], *, preserve_existing: bool = False) -> None:
    source = read_ini(stage_symbols)
    destination = read_ini(copied_symbols) if copied_symbols.is_file() else ini_parser()
    required = {"host"}
    for manifest in manifests:
        required.update(manifest_requires(manifest))
    missing = sorted(section for section in required if not source.has_section(section) and
                     not (preserve_existing and destination.has_section(section)))
    if missing:
        raise GameTestError(f"stage host symbols lack required sections: {', '.join(missing)}")
    for section in sorted(required):
        if not source.has_section(section):continue
        if destination.has_section(section):
            destination.remove_section(section)
        destination.add_section(section)
        for key, value in source.items(section, raw=True):
            destination.set(section, key, value)
    copied_symbols.parent.mkdir(parents=True, exist_ok=True)
    write_ini(copied_symbols, destination)


def enable_test_configuration(root: Path) -> None:
    manifest = root / "sdgo" / "mods" / TEST_MOD / "mod.ini"
    parser = read_ini(manifest)
    if not parser.has_section("mod") or parser["mod"].get("id") != TEST_MOD:
        raise GameTestError(f"invalid staged observer manifest: {manifest}")
    parser["mod"]["enabled"] = "1"
    write_ini(manifest, parser)

    settings_path = root / "sdgo" / "sdgo.ini"
    settings = read_ini(settings_path)
    for section in ("categories", "features", "mods"):
        if not settings.has_section(section):
            settings.add_section(section)
    settings["categories"]["development"] = "1"
    settings["categories"]["gameplay"] = "1"
    settings["features"]["game-test"] = "1"
    settings["features"]["pvp-ai"] = "1"
    settings["mods"][TEST_MOD] = "1"
    settings["mods"][TARGET_MOD] = "1"
    write_ini(settings_path, settings)


def overlay_observer(root: Path, stage: Path) -> None:
    stage_client = stage / "client"
    observer_source = stage_client / "sdgo" / "mods" / TEST_MOD
    if not (observer_source / f"{TEST_MOD}.dll").is_file() or not (observer_source / "mod.ini").is_file():
        raise GameTestError(f"stage lacks native observer Mod: {observer_source}")

    copy_file_if_missing(stage_client / "d3d9.dll", root / "d3d9.dll")
    # The current test Mod can require a newer manifest loader. Bind the
    # staged core in this isolated copy; leave the normal template untouched.
    staged_core=stage_client / "sdgo" / "core.dll"
    if not staged_core.is_file():
        raise GameTestError(f"stage lacks framework core: {staged_core}")
    shutil.copy2(staged_core,root / "sdgo" / "core.dll")
    copy_file_if_missing(stage_client / "sdgo" / "sdgo.ini", root / "sdgo" / "sdgo.ini")

    observer_target = root / "sdgo" / "mods" / TEST_MOD
    if observer_target.exists():
        shutil.rmtree(observer_target)
    shutil.copytree(observer_source, observer_target)

    target_manifest = root / "sdgo" / "mods" / TARGET_MOD / "mod.ini"
    target_dll = root / "sdgo" / "mods" / TARGET_MOD / f"{TARGET_MOD}.dll"
    if not target_manifest.is_file() or not target_dll.is_file():
        raise GameTestError(
            "normal copied client lacks match_ai; install the normal compiled target before preparing GameTest"
        )
    staged_target_dll = (
        stage_client / "sdgo" / "mods" / TARGET_MOD / f"{TARGET_MOD}.dll"
    )
    if staged_target_dll.is_file():
        staged_target_manifest=staged_target_dll.with_name('mod.ini')
        if not staged_target_manifest.is_file():
            raise GameTestError('staged production match_ai DLL requires its matching manifest')
        previous_manifest=read_ini(target_manifest)
        current_manifest=read_ini(staged_target_manifest)
        if current_manifest.get('mod','id',fallback='')!=TARGET_MOD:
            raise GameTestError('staged production match_ai manifest has the wrong identity')
        if previous_manifest.has_section('settings'):
            if not current_manifest.has_section('settings'):current_manifest.add_section('settings')
            for key,value in previous_manifest.items('settings',raw=True):
                current_manifest['settings'][key]=value
        write_ini(target_manifest,current_manifest)
        shutil.copy2(staged_target_dll, target_dll)

    # Exercise the built master implementation of every already-installed Mod,
    # including its Lua policy, while keeping private owner settings local.
    # An unrelated installed addon without a staged counterpart is preserved.
    updated_manifests=[observer_target/'mod.ini',target_manifest]
    for staged_manifest in sorted((stage_client/'sdgo/mods').glob('*/mod.ini')):
        identifier=staged_manifest.parent.name
        if identifier in (TEST_MOD,TARGET_MOD):continue
        destination=root/'sdgo/mods'/identifier
        installed_manifest=destination/'mod.ini'
        if not installed_manifest.is_file():continue
        current=read_ini(staged_manifest)
        if current.get('mod','category',fallback='')=='development':continue
        previous=read_ini(installed_manifest)
        if current.get('mod','id',fallback='')!=identifier:
            raise GameTestError(f'staged Mod identity mismatch: {identifier}')
        if previous.has_section('settings'):
            if not current.has_section('settings'):current.add_section('settings')
            for key,value in previous.items('settings',raw=True):current['settings'][key]=value
        write_ini(installed_manifest,current)
        updated_manifests.append(installed_manifest)
        for payload in staged_manifest.parent.iterdir():
            if payload.is_file() and payload.suffix.lower() in ('.dll','.lua'):
                shutil.copy2(payload,destination/payload.name)

    stage_symbols = stage_client / "sdgo" / "symbols" / f"{HOST_KEY}.ini"
    copied_symbols = root / "sdgo" / "symbols" / f"{HOST_KEY}.ini"
    if not stage_symbols.is_file():
        raise GameTestError(f"stage lacks host symbols: {stage_symbols}")
    merge_required_symbols(
        stage_symbols,
        copied_symbols,
        updated_manifests,
    )
    retained_manifests=[manifest for manifest in (root/'sdgo/mods').glob('*/mod.ini') if manifest not in updated_manifests]
    merge_required_symbols(stage_symbols,copied_symbols,retained_manifests,preserve_existing=True)
    enable_test_configuration(root)


def configure_sfx_only(root: Path) -> dict:
    """Change only the prepared client's audio settings before hashing it."""
    path = root / "config.xml"
    try:
        tree = ET.parse(path)
        sound = tree.getroot().find("Sound")
        if sound is None:
            raise ValueError("missing Sound settings")
        bgm = sound.find("BGMVolume")
        effects = sound.find("EffectVolume")
        if bgm is None or effects is None:
            raise ValueError("missing BGMVolume or EffectVolume")
        previous_effects = int(effects.get("Value", "0"))
        bgm.set("Value", "0")
        bgm.set("use", "false")
        effects.set("use", "true")
        effects.set("Value", str(previous_effects if 0 < previous_effects <= 100 else 40))
        tree.write(path, encoding="utf-8", xml_declaration=True)
        return {"audio": "sfx_only", "bgm_volume": 0,
                "effect_volume": int(effects.get("Value")), "process_audio_only": True}
    except (OSError, ET.ParseError, ValueError) as error:
        raise GameTestError(f"cannot configure SFX-only test client: {error}") from error


def configure_muted_audio(root: Path) -> dict:
    path=root/'config.xml'
    try:
        tree=ET.parse(path)
        video=tree.getroot().find('Video')
        if video is None:video=ET.SubElement(tree.getroot(),'Video')
        screen=video.find('ScreenMode')
        if screen is None:screen=ET.SubElement(video,'ScreenMode',{'width':'1024','height':'768','bpp':'32'})
        screen.set('mode','window');screen.set('windowFull','0')
        sound=tree.getroot().find('Sound')
        if sound is None:raise ValueError('missing Sound settings')
        for key in ('BGMVolume','EffectVolume','VoiceVolume'):
            setting=sound.find(key)
            if setting is None:raise ValueError(f'missing {key}')
            # Keep the legacy sound-driver objects initialized. Zero volume
            # silences the copy without disabling their native lifecycle.
            setting.set('Value','0');setting.set('use','true')
        tree.write(path,encoding='utf-8',xml_declaration=True)
        return {'audio':'muted','bgm_volume':0,'effect_volume':0,'voice_volume':0}
    except (OSError,ET.ParseError,ValueError) as error:
        raise GameTestError(f'cannot prepare a silent test client: {error}') from error


def validate_muted_audio(root: Path) -> None:
    try:
        document=ET.parse(root/'config.xml').getroot()
        screen=document.find('Video/ScreenMode')
        if screen is None or screen.get('mode')!='window' or screen.get('windowFull')!='0':
            raise ValueError('background tests must use a windowed device and may not change the display mode')
        sound=document.find('Sound')
        if sound is None or any(sound.find(key) is None or
            sound.find(key).get('Value')!='0' or sound.find(key).get('use') not in ('true','false')
            for key in ('BGMVolume','EffectVolume','VoiceVolume')):
            raise ValueError('all three test audio channels must have zero volume')
    except (OSError,ET.ParseError,ValueError) as error:
        raise GameTestError(f'quiet test audio policy failed before launch: {error}') from error


class QuietRuntime:
    def __init__(self,metadata: dict,receipt_dir: Path):
        self.metadata=metadata;self.directory=receipt_dir;self.process=None;self.firewall=None;self.firewall_attempted=False
        self.proof_file_written=False
        self.receipt={'status':'preparing','audio':'muted','background':True}

    def firewall_action(self,action: str) -> dict:
        # Argument-array invocation, no UAC/elevation or user-visible console.
        result=subprocess.run(['powershell.exe','-NoProfile','-NonInteractive','-WindowStyle','Hidden',
            '-ExecutionPolicy','Bypass','-File',str(Path(__file__).with_name('game_test_firewall.ps1')),
            '-Action',action,'-Program',self.metadata['client']['game'],'-RunId',self.metadata['run_id'],
            '-RemoteIPv4',self.metadata['plan']['target_host']],capture_output=True,text=True,
            creationflags=getattr(subprocess,'CREATE_NO_WINDOW',0),timeout=30)
        if result.returncode:raise GameTestError('cannot establish the owned silent firewall rule; client was not launched interactively')
        try:return json.loads(result.stdout)
        except json.JSONDecodeError as error:raise GameTestError('invalid quiet firewall readback') from error

    def start(self) -> None:
        validate_muted_audio(Path(self.metadata['client']['root']))
        # Scope stays on the declared server. Routed/non-IPv4 tests must supply
        # an explicit endpoint before a quiet run can open a native listener.
        try:ipaddress.IPv4Address(self.metadata['plan']['target_host'])
        except ipaddress.AddressValueError as error:raise GameTestError('quiet tests require an explicit IPv4 server endpoint') from error
        if (ipaddress.IPv4Address(self.metadata['plan']['target_host']).is_loopback and
            self.metadata['plan'].get('network_policy')=='direct'):
            # Local GameTest has no reason to listen on a public interface.
            # The native Mod constrains its own bind calls before game startup.
            self.firewall={'status':'loopback_only','system_policy_changed':False}
        else:
            self.firewall_attempted=True
            self.firewall=self.firewall_action('Install')
        self.receipt['firewall']=self.firewall
        self.save()

    def save(self):
        atomic_write(self.directory/'quiet-runtime.json',json.dumps(self.receipt,indent=2))

    def spawn(self,arguments: list[str],**kwargs):
        spec=importlib.util.spec_from_file_location('sdgo_game_test_windows',Path(__file__).with_name('game_test_windows.py'))
        module=importlib.util.module_from_spec(spec);sys.modules[spec.name]=module;spec.loader.exec_module(module)
        self.process=module.WindowsBackgroundProcess(arguments,cwd=kwargs['cwd'],env=kwargs['env'],
            stdout=kwargs['stdout'],stderr=kwargs['stderr'],run_id=self.metadata['run_id'])
        return self.process

    def verify(self,pid: int) -> dict | None:
        marker=Path(self.metadata['client']['root'])/'sdgo/gametest/quiet-sockets.ini'
        guard=read_ini(marker) if marker.is_file() else None
        matching=guard is not None and guard.get('run','id',fallback='')==self.metadata['run_id'] and guard.getint('policy','pid',fallback=0)==pid
        proxy=guard.getint('graphics','proxy_hwnd',fallback=0) if matching else 0
        proof=self.process.verify_background(pid,allowed_input_windows=(proxy,) if proxy else ())
        if self.receipt.get('desktop_probe')!=proof:
            self.receipt['desktop_probe']=proof
            self.save()
        if not proof.get('foreground_safe',False):raise GameTestError('owned game attempted to take the user foreground')
        if not proof.get('verified',False):return None
        if not matching:return None
        if (not proxy or guard.getint('graphics','active',fallback=0)!=1 or guard.getint('graphics','worker_ready',fallback=0)!=1 or
            guard.getint('graphics','created_devices',fallback=0)<1):return None
        if guard.getint('graphics','dispatch_failures',fallback=0):raise GameTestError('native background graphics dispatch failed')
        proof['graphics_backend']='native_d3d9_hal_hidden_proxy'
        opened=guard.getint('audio','opened',fallback=0)
        if guard.getint('audio','active',fallback=0)!=1 or opened<1:return None
        if guard.getint('audio','muted',fallback=0)!=opened or guard.getint('audio','failed',fallback=0):
            raise GameTestError('native Miles audio drivers did not verify zero master volume')
        proof.update(audio_driver='native_miles_waveout',audio_drivers_opened=opened,audio_drivers_muted=opened)
        network_verified=self.firewall.get('status')=='installed'
        if self.firewall.get('status')=='loopback_only':
            if (guard.getint('policy','loopback_only',fallback=0)!=1 or
                guard.getint('policy','active',fallback=0)!=1):return None
            if guard.getint('sockets','escaped',fallback=0):raise GameTestError('native socket escaped the test loopback interface')
            endpoints=owned_udp_endpoints(pid)
            if any(not ipaddress.IPv4Address(row['local_ipv4']).is_loopback for row in endpoints):
                raise GameTestError('owned game has a non-loopback UDP listener')
            network_verified=True
            proof.update(network_policy='loopback_only',udp_endpoints=endpoints)
        proof={**proof,'audio_muted':True,'firewall_verified':network_verified,'pid':pid}
        if self.receipt.get('proof')!=proof:
            self.receipt.update(status='running',proof=proof);self.save()
        if not self.proof_file_written:
            parser=ini_parser();parser['run']={'id':self.metadata['run_id']}
            parser['quiet']={'pid':str(pid),'verified':'1','audio_muted':'1','firewall_verified':'1'}
            write_ini(Path(self.metadata['client']['root'])/'sdgo/gametest/quiet-verified.ini',parser)
            self.proof_file_written=True
        return proof

    def close(self) -> dict:
        errors=[]
        if self.process:
            try:self.process.close()
            except Exception as error:errors.append(type(error).__name__)
        if self.firewall_attempted:
            try:self.receipt['cleanup']=self.firewall_action('Remove')
            except Exception as error:errors.append(type(error).__name__)
        elif self.firewall:
            self.receipt['cleanup']={'status':'removed','system_policy_changed':False}
        self.receipt['status']='failed' if errors else 'closed'
        self.receipt['cleanup_errors']=errors;self.save();return self.receipt


def random_account() -> tuple[str, str]:
    return (
        "".join(secrets.choice(ALNUM) for _ in range(10)),
        "".join(secrets.choice(ALNUM) for _ in range(12)),
    )


def configure_test_network(root: Path, policy: str) -> None:
    if policy == 'preserve':return
    if policy != 'direct':raise GameTestError('unknown test network policy')
    # Owner routing can override explicit command-line endpoints. Only the
    # copied translator is disabled; the source client is never changed.
    manifest = root/'sdgo/mods/net_route/mod.ini'
    if manifest.exists():
        config=read_ini(manifest);config['mod']['enabled']='0';write_ini(manifest,config)
    path=root/'sdgo/sdgo.ini';settings=read_ini(path)
    if not settings.has_section('mods'):settings.add_section('mods')
    settings['mods']['net_route']='0';write_ini(path,settings)


def owned_tcp_peers(pid: int) -> list[dict]:
    if os.name != 'nt':return []
    api=ctypes.WinDLL('iphlpapi',use_last_error=True)
    get=api.GetExtendedTcpTable
    get.argtypes=[ctypes.c_void_p,ctypes.POINTER(wintypes.DWORD),wintypes.BOOL,wintypes.ULONG,ctypes.c_int,wintypes.ULONG]
    get.restype=wintypes.DWORD
    size=wintypes.DWORD(0)
    result=get(None,ctypes.byref(size),False,2,5,0)
    if result not in (0,122) or not 4<=size.value<=4*1024*1024:return []
    data=ctypes.create_string_buffer(size.value)
    if get(data,ctypes.byref(size),False,2,5,0):return []
    count=struct.unpack_from('<I',data.raw)[0]
    if count>(size.value-4)//24:return []
    peers=[]
    for i in range(count):
        state,local,local_port,remote,remote_port,owner=struct.unpack_from('<6I',data.raw,4+i*24)
        if owner==pid and state==5:
            peers.append({'local_ipv4':socket.inet_ntoa(struct.pack('<I',local)),
                'local_port':socket.ntohs(local_port&65535),'remote_ipv4':socket.inet_ntoa(struct.pack('<I',remote)),
                'remote_port':socket.ntohs(remote_port&65535)})
    return peers


def verify_direct_peer(metadata: dict, pid: int) -> dict | None:
    expected=metadata['plan']['target_host'];port=metadata['plan']['login_port']
    peers=owned_tcp_peers(pid)
    match=next((p for p in peers if p['remote_ipv4']==expected and p['remote_port']==port),None)
    if match is None:return None
    proof={'verified':True,'pid':pid,'remote_ipv4':expected,'remote_port':port,'observed':peers}
    receipt=ini_parser();receipt['run']={'id':metadata['run_id']}
    receipt['network']={'verified':'1','pid':str(pid),'ipv4':expected,'port':str(port)}
    write_ini(Path(metadata['client']['root'])/'sdgo/gametest/network-verified.ini',receipt)
    return proof


def owned_udp_endpoints(pid: int) -> list[dict]:
    if os.name!='nt':raise GameTestError('quiet socket readback requires Windows')
    api=ctypes.WinDLL('iphlpapi',use_last_error=True)
    get=api.GetExtendedUdpTable
    get.argtypes=[ctypes.c_void_p,ctypes.POINTER(wintypes.DWORD),wintypes.BOOL,wintypes.ULONG,ctypes.c_int,wintypes.ULONG]
    get.restype=wintypes.DWORD
    size=wintypes.DWORD(0)
    result=get(None,ctypes.byref(size),False,2,1,0)
    if result not in (0,122) or not 4<=size.value<=4*1024*1024:
        raise GameTestError('cannot size the Windows process-owned UDP table')
    data=ctypes.create_string_buffer(size.value)
    if get(data,ctypes.byref(size),False,2,1,0):raise GameTestError('cannot read the Windows process-owned UDP table')
    count=struct.unpack_from('<I',data.raw)[0]
    if count>(size.value-4)//12:raise GameTestError('malformed Windows process-owned UDP table')
    rows=[]
    for i in range(count):
        local,port,owner=struct.unpack_from('<3I',data.raw,4+i*12)
        if owner==pid:rows.append({'local_ipv4':socket.inet_ntoa(struct.pack('<I',local)),
                                 'local_port':socket.ntohs(port&65535)})
    return rows


def default_account_profile() -> Path:
    local_app_data = os.environ.get("LOCALAPPDATA")
    if not local_app_data:
        raise GameTestError(
            "LOCALAPPDATA is unavailable; pass --account-profile with the persistent test account"
        )
    return Path(local_app_data) / "SDGO-GameTest" / "account.ini"


def load_account_profile(path: Path) -> configparser.ConfigParser:
    if not path.is_file():
        raise GameTestError(
            f"persistent test account profile is missing: {path}; "
            "promote the already-registered account or explicitly use --create-test-account once"
        )
    parser = read_ini(path)
    try:
        username = parser["account"]["username"]
        password = parser["account"]["password"]
    except KeyError as error:
        raise GameTestError(f"persistent test account profile is invalid: {path}") from error
    validate_account_credentials(parser, username, password)
    return parser


def validate_account_credentials(parser, username: str, password: str) -> None:
    existing = parser.get('registration', 'state', fallback='') == 'existing_account'
    password_format = parser.get('account', 'password_format', fallback='plain')
    if existing:
        if not re.fullmatch(r'[A-Za-z0-9_]{1,14}', username):
            raise GameTestError('existing account username exceeds the native ASCII account field')
        if password_format == 'md5':
            if not re.fullmatch(r'[A-Fa-f0-9]{32}', password):
                raise GameTestError('existing account MD5 credential must contain exactly 32 hex characters')
        elif password_format == 'plain':
            if not password or len(password) > 64 or not password.isascii() or any(c in password for c in '\r\n\0'):
                raise GameTestError('existing account password does not fit the native account input')
        else:
            raise GameTestError('unknown existing account password format')
    else:
        if password_format != 'plain':
            raise GameTestError('imported MD5 credentials require an explicitly existing account profile')
        if not re.fullmatch(r'[A-Za-z0-9]{10}', username):
            raise GameTestError('persistent test account username must be 10 ASCII alphanumeric characters')
        if not re.fullmatch(r'[A-Za-z0-9]{12}', password):
            raise GameTestError('persistent test account password must be 12 ASCII alphanumeric characters')


def create_account_profile(path: Path) -> configparser.ConfigParser:
    if path.exists():
        raise GameTestError(f"refusing to overwrite persistent test account profile: {path}")
    username, password = random_account()
    parser = ini_parser()
    parser["account"] = {"username": username, "password": password}
    parser["registration"] = {"state": "pending_first_login"}
    path.parent.mkdir(parents=True, exist_ok=True)
    write_ini(path, parser)
    os.chmod(path, stat.S_IREAD | stat.S_IWRITE)
    return parser


def validate_public_value(label: str, value: str) -> str:
    if not value or len(value) > 128 or any(character in "\r\n\x00" for character in value):
        raise GameTestError(f"invalid {label}")
    return value


def parse_mode_sequence(value: str) -> tuple[int, ...]:
    value = validate_public_value("mode_sequence", value)
    if not re.fullmatch(r"[13679](?:,[13679])*", value):
        supported = ",".join(str(mode) for mode in SUPPORTED_PVP_MODES)
        raise GameTestError(
            "mode_sequence must contain comma-separated single-digit PvP modes "
            f"without spaces or leading zeros; supported modes: {supported}"
        )
    modes = tuple(int(item) for item in value.split(","))
    if len(set(modes)) != len(modes):
        raise GameTestError("mode_sequence must not contain duplicate modes")
    return modes


def required_flow_cases(mode_sequence: str, bootstrap_only: bool = False,
                        battleship_only: bool = False, gift_requests_only: bool = False,
                        deck_only: bool = False) -> tuple[str, ...]:
    modes = parse_mode_sequence(mode_sequence)
    if sum((bool(bootstrap_only),bool(battleship_only),bool(gift_requests_only),bool(deck_only))) > 1:
        raise GameTestError('choose only one account preparation operation')
    if bootstrap_only:
        return FLOW_CASES[:3]
    if gift_requests_only:
        return FLOW_CASES[:3] + ('setup.gift_requests',)
    if deck_only:
        return FLOW_CASES[:3] + ('flow.battleship','setup.deck')
    prefix = FLOW_CASES[:3]
    if battleship_only or any(mode in (6,7) for mode in modes):
        prefix += ('flow.battleship',)
    if battleship_only:
        return prefix
    if len(modes) == 1:
        return prefix + FLOW_CASES[3:]
    return prefix + tuple(
        f"pvp.mode{mode}.{step}" for mode in modes for step in MODE_FLOW_STEPS
    )


def account_profile_lock_path(path: Path) -> Path:
    return path.with_name(path.name + ".run.lock")


def make_plan(
    target_host: str,
    login_port: int,
    target_server: str,
    target_channel: str,
    mode_sequence: str,
    retry_count: int,
    expect_first_fail: bool,
    expected_ai: int,
    launcher_path: str | None,
) -> dict:
    if not 1 <= login_port <= 65535:
        raise GameTestError("login_port must be between 1 and 65535")
    if retry_count < 1 or retry_count > 2:
        raise GameTestError("retry_count must be 1 or 2")
    if expect_first_fail and retry_count != 2:
        raise GameTestError("expect_first_fail requires retry_count=2")
    if expected_ai < 1 or expected_ai > 7:
        raise GameTestError("expected_ai must be between 1 and 7")
    modes = parse_mode_sequence(mode_sequence)
    title_nonce = "gt" + secrets.token_hex(2)
    enemies = (expected_ai + 1) // 2
    allies = expected_ai - enemies
    plan = {
        "target_host": validate_public_value("target_host", target_host),
        "login_port": login_port,
        "target_server": validate_public_value("target_server", target_server),
        "target_channel": validate_public_value("target_channel", target_channel),
        "server_channel_selection": "native_automatic",
        "mode_sequence": ",".join(str(mode) for mode in modes),
        "retry_count": retry_count,
        "expect_first_fail": bool(expect_first_fail),
        "expected_ai": expected_ai,
        "title_nonce": title_nonce,
        "room_title": f"{title_nonce} !ai e{enemies} a{allies}",
        "auto_nickname": "account_name",
        "exit_tutorial": True,
        "first_unit_draw": True,
        "acknowledge_game_windows": True,
        "launcher_path": launcher_path or "",
    }
    if launcher_path:
        relative = Path(launcher_path)
        if relative.is_absolute() or ".." in relative.parts:
            raise GameTestError("launcher_path must be a safe path relative to the copied client")
    return plan


def write_job(
    root: Path,
    run_id: str,
    plan: dict,
    account_profile: configparser.ConfigParser,
    profile_path: Path,
) -> None:
    directory = root / "sdgo" / "gametest"
    private = directory / "private"
    private.mkdir(parents=True)

    account = ini_parser()
    for section in account_profile.sections():
        account.add_section(section)
        for key, value in account_profile.items(section, raw=True):
            account.set(section, key, value)
    if not account.has_section("registration"):
        account.add_section("registration")
    account["registration"] = {
        **dict(account["registration"]),
        "retry_count": str(plan["retry_count"]),
        "expect_first_fail": "1" if plan["expect_first_fail"] else "0",
    }
    account["runner"] = {
        "lock_path": str(account_profile_lock_path(profile_path)),
    }
    account_path = private / "account.ini"
    write_ini(account_path, account)
    os.chmod(account_path, stat.S_IREAD | stat.S_IWRITE)

    run = ini_parser()
    run["run"] = {
        "id": run_id,
        "role": "client",
        "suite": "pvp-ai",
        "normal_flow": "1",
        "endpoint": f"{plan['target_host']}:{plan['login_port']}",
        "target_server": plan["target_server"],
        "target_channel": plan["target_channel"],
        "server_channel_selection": plan["server_channel_selection"],
        "mode_sequence": plan["mode_sequence"],
        "title_nonce": plan["title_nonce"],
        "room_title": plan["room_title"],
        "expected_ai": str(plan["expected_ai"]),
        "auto_nickname": plan["auto_nickname"],
        "exit_tutorial": "1" if plan["exit_tutorial"] else "0",
        "first_unit_draw": "1" if plan["first_unit_draw"] else "0",
        "acknowledge_game_windows": "1" if plan["acknowledge_game_windows"] else "0",
        "process_entry": plan["launcher_path"] or GAME_EXE,
        "account_path": "private/account.ini",
        "retry_count": str(plan["retry_count"]),
        "expect_first_fail": "1" if plan["expect_first_fail"] else "0",
        "required_cases": ",".join(required_flow_cases(plan["mode_sequence"], plan.get('bootstrap_only', False),
            plan.get('battleship_only',False),plan.get('gift_requests_only',False),plan.get('deck_only',False))),
    }
    for flag in ('bootstrap_only','battleship_only','gift_requests_only','deck_only'):
        if plan.get(flag):run['run'][flag] = '1'
    if 'network_policy' in plan:run['run']['network_policy']=plan['network_policy']
    if 'quiet' in plan:run['run']['quiet']='1' if plan['quiet'] else '0'
    if plan.get('quiet'):
        run['run']['quiet_loopback']='1' if (plan.get('network_policy')=='direct' and
            ipaddress.IPv4Address(plan['target_host']).is_loopback) else '0'
    write_ini(directory / "run.ini", run)


def read_public_plan(root: Path) -> dict:
    parser = read_ini(root / "sdgo" / "gametest" / "run.ini")
    try:
        run = parser["run"]
        mode_sequence = ",".join(str(mode) for mode in parse_mode_sequence(run["mode_sequence"]))
        bootstrap_only = run.get('bootstrap_only', '0') == '1'
        battleship_only = run.get('battleship_only', '0') == '1'
        gift_requests_only = run.get('gift_requests_only', '0') == '1'
        deck_only = run.get('deck_only', '0') == '1'
        if any(run.get(flag,'0') not in ('0','1') for flag in ('bootstrap_only','battleship_only','gift_requests_only','deck_only')):
            raise ValueError('invalid preparation flag')
        if (
            run["role"] != "client"
            or run["suite"] != "pvp-ai"
            or run["normal_flow"] != "1"
            or run["server_channel_selection"] != "native_automatic"
            or run["account_path"] != "private/account.ini"
            or run["required_cases"] != ",".join(required_flow_cases(mode_sequence, bootstrap_only,battleship_only,gift_requests_only,deck_only))
            or run["auto_nickname"] != "account_name"
            or run["exit_tutorial"] != "1"
            or run["first_unit_draw"] != "1"
            or run["acknowledge_game_windows"] != "1"
        ):
            raise ValueError("run contract does not describe the required normal client flow")
        host, port = run["endpoint"].rsplit(":", 1)
        process_entry = run["process_entry"]
        plan = {
            "target_host": host,
            "login_port": int(port),
            "target_server": run["target_server"],
            "target_channel": run["target_channel"],
            "server_channel_selection": run["server_channel_selection"],
            "mode_sequence": mode_sequence,
            "retry_count": int(run["retry_count"]),
            "expect_first_fail": run["expect_first_fail"] == "1",
            "expected_ai": int(run["expected_ai"]),
            "title_nonce": run["title_nonce"],
            "room_title": run["room_title"],
            "auto_nickname": run["auto_nickname"],
            "exit_tutorial": run["exit_tutorial"] == "1",
            "first_unit_draw": run["first_unit_draw"] == "1",
            "acknowledge_game_windows": run["acknowledge_game_windows"] == "1",
            "launcher_path": "" if process_entry == GAME_EXE else process_entry,
        }
        for flag,value in (('bootstrap_only',bootstrap_only),('battleship_only',battleship_only),('gift_requests_only',gift_requests_only),('deck_only',deck_only)):
            if value:plan[flag] = True
        if 'network_policy' in run:
            if run['network_policy'] not in ('direct','preserve'):raise ValueError('invalid network policy')
            plan['network_policy']=run['network_policy']
        if 'quiet' in run:
            if run['quiet'] not in ('0','1'):raise ValueError('invalid quiet policy')
            plan['quiet']=run['quiet']=='1'
        return plan
    except (KeyError, ValueError) as error:
        raise GameTestError(f"invalid normal-flow run.ini: {error}") from error


def validate_deck_profile(parser: configparser.ConfigParser) -> None:
    try:
        values = [parser['deck'][f'unit_uid{i}'] for i in range(4)]
        if any(not re.fullmatch(r'[0-9]+',value) or not 0 < int(value) < 2**64 for value in values):
            raise ValueError('invalid unit UID')
        if len({int(value) for value in values}) != 4:
            raise ValueError('duplicate unit UID')
    except (KeyError,ValueError) as error:
        raise GameTestError('deck preparation requires four distinct real unit UIDs in the private deck profile') from error


def validate_private_account(root: Path, plan: dict) -> Path:
    path = root / "sdgo" / "gametest" / "private" / "account.ini"
    parser = read_ini(path)
    try:
        username = parser["account"]["username"]
        password = parser["account"]["password"]
        retry_count = int(parser["registration"]["retry_count"])
        expect_first_fail = parser["registration"]["expect_first_fail"] == "1"
        raw_lock_path = parser["runner"]["lock_path"]
    except (KeyError, ValueError) as error:
        raise GameTestError(f"invalid private GameTest account file: {error}") from error
    validate_account_credentials(parser, username, password)
    if plan.get('deck_only'):
        validate_deck_profile(parser)
    if parser.get('registration', 'state', fallback='') == 'existing_account' and (retry_count != 1 or expect_first_fail):
        raise GameTestError('existing account imports cannot use registration retries')
    if retry_count != plan["retry_count"] or expect_first_fail != plan["expect_first_fail"]:
        raise GameTestError("private GameTest registration plan disagrees with run.ini")
    lock_path = Path(raw_lock_path)
    if not lock_path.is_absolute() or not lock_path.name.endswith(".run.lock"):
        raise GameTestError("private GameTest account lock path is invalid")
    return lock_path.resolve()


def collect_hashes(root: Path) -> tuple[dict, dict]:
    immutable: dict[str, str] = {}
    initial: dict[str, str] = {}

    def bind(path: Path, collection: dict) -> None:
        if not path.is_file():
            raise GameTestError(f"missing copied GameTest payload: {path}")
        collection[path.relative_to(root).as_posix()] = sha256(path)

    bind(root / GAME_EXE, immutable)
    for path in sorted(root.iterdir(), key=lambda item: item.name.lower()):
        if path.is_file() and path.suffix.lower() in IMMUTABLE_ROOT_SUFFIXES:
            bind(path, immutable)
    for path in (
        root / "sdgo" / "core.dll",
        root / "sdgo" / "sdgo.ini",
        root / "sdgo" / "gametest" / "run.ini",
    ):
        bind(path, immutable)
    symbols = root / "sdgo" / "symbols"
    for path in sorted(symbols.iterdir(), key=lambda item: item.name.lower()):
        if path.is_file() and path.suffix.lower() == ".ini":
            bind(path, immutable)
    mods = root / "sdgo" / "mods"
    for path in sorted(mods.glob("*/*"), key=lambda item: item.as_posix().lower()):
        if path.is_file() and (path.name.lower() == "mod.ini" or path.suffix.lower() in {".dll", ".lua"}):
            bind(path, immutable)
    for name in ("config.xml", "配置文件.ini"):
        path = root / name
        if path.is_file():
            bind(path, initial)
    return immutable, initial


def _remove_owned_preparation(path: Path, token: str) -> str:
    if not path.is_absolute() or path.name != f".gametest-preparing-{token}":
        return f"retained unverified preparation path: {path}"
    if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
        return f"retained linked preparation path: {path}"
    marker = path / ".gametest-preparing.json"
    if marker.is_symlink():
        return f"retained preparation with linked marker: {path}"
    try:
        data = read_json(marker)
        target = Path(data["target"]).resolve()
        resolved = path.resolve(strict=True)
    except (GameTestError, KeyError, OSError):
        return f"retained preparation with unverifiable marker: {path}"
    if data.get("token") != token or resolved.parent != target.parent:
        return f"retained preparation outside its verified target parent: {path}"
    return f"retained owned preparation for recoverable Recycle Bin cleanup: {path}"


def rename_prepared_directory(source: Path, target: Path, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + max(0.0, timeout)
    delay = 0.05
    while True:
        if target.exists():
            raise GameTestError(f"refusing to overwrite existing run directory: {target}")
        try:
            os.replace(source, target)
            return
        except PermissionError as error:
            if getattr(error, "winerror", None) not in {5, 32}:
                raise GameTestError(f"cannot finalize GameTest directory: {error}") from error
            if time.monotonic() >= deadline:
                raise GameTestError(
                    f"cannot finalize GameTest directory after {timeout:g}s of Windows file-lock retries: {error}"
                ) from error
            time.sleep(min(delay, max(0.0, deadline - time.monotonic())))
            delay = min(delay * 2.0, 0.5)
        except OSError as error:
            raise GameTestError(f"cannot finalize GameTest directory: {error}") from error


def prepare(
    client_template: Path,
    stage: Path,
    run_dir: Path,
    *,
    target_host: str = "127.0.0.1",
    login_port: int = 5001,
    target_server: str = "1",
    target_channel: str = "1",
    mode_sequence: str = "1",
    retry_count: int | None = None,
    expect_first_fail: bool | None = None,
    expected_ai: int = 3,
    launcher_path: str | None = None,
    account_profile: Path | None = None,
    create_test_account: bool = False,
    sfx_only: bool = False,
    bootstrap_only: bool = False,
    battleship_only: bool = False,
    gift_requests_only: bool = False,
    deck_only: bool = False,
    network_policy: str = 'direct',
    quiet: bool = True,
) -> dict:
    client_template = client_template.resolve()
    if quiet and sfx_only:raise GameTestError('SFX capture requires explicit --visible; ordinary tests are background and silent')
    if quiet:
        try:ipaddress.IPv4Address(target_host)
        except ipaddress.AddressValueError as error:raise GameTestError('quiet tests require an explicit IPv4 server endpoint') from error
    if network_policy not in ('direct','preserve'):raise GameTestError('invalid network policy')
    if network_policy=='direct':
        try:target_host=str(ipaddress.IPv4Address(target_host))
        except ipaddress.AddressValueError as error:raise GameTestError('direct tests require an explicit IPv4 login endpoint') from error
    stage = stage.resolve()
    run_dir = run_dir.resolve()
    if not client_template.is_dir() or not (client_template / GAME_EXE).is_file():
        raise GameTestError(f"client template lacks {GAME_EXE}: {client_template}")
    if not (stage / "client").is_dir():
        raise GameTestError(f"stage lacks client tree: {stage / 'client'}")
    if run_dir.exists():
        raise GameTestError(f"run directory already exists: {run_dir}")
    if is_relative_to(run_dir, client_template):
        raise GameTestError("run directory cannot be inside the client template")

    profile_path = (account_profile or default_account_profile()).expanduser().resolve()
    if create_test_account:
        resolved_retry_count = 2 if retry_count is None else retry_count
        resolved_expect_first_fail = True if expect_first_fail is None else expect_first_fail
        if resolved_retry_count != 2 or not resolved_expect_first_fail:
            raise GameTestError(
                "--create-test-account requires retry_count=2 and expect_first_fail=true"
            )
        profile = create_account_profile(profile_path)
    else:
        profile = load_account_profile(profile_path)
        pending_first_login = (
            profile.get("registration", "state", fallback="").strip()
            == "pending_first_login"
        )
        default_retry_count = 2 if pending_first_login else 1
        resolved_retry_count = default_retry_count if retry_count is None else retry_count
        resolved_expect_first_fail = False if expect_first_fail is None else expect_first_fail
        if profile.get('registration', 'state', fallback='') == 'existing_account' and (resolved_retry_count != 1 or resolved_expect_first_fail):
            raise GameTestError('existing account imports cannot use registration retries')

    if deck_only:
        validate_deck_profile(profile)
    plan = make_plan(
        target_host,
        login_port,
        target_server,
        target_channel,
        mode_sequence,
        resolved_retry_count,
        resolved_expect_first_fail,
        expected_ai,
        launcher_path,
    )
    required_flow_cases(mode_sequence,bootstrap_only,battleship_only,gift_requests_only,deck_only)
    plan['network_policy']=network_policy
    plan['quiet']=quiet
    for flag,value in (('bootstrap_only',bootstrap_only),('battleship_only',battleship_only),('gift_requests_only',gift_requests_only),('deck_only',deck_only)):
        if value:plan[flag] = True
    run_dir.parent.mkdir(parents=True, exist_ok=True)
    token = uuid.uuid4().hex
    preparing = run_dir.parent / f".gametest-preparing-{token}"
    preparing.mkdir()
    atomic_write(
        preparing / ".gametest-preparing.json",
        json.dumps({"token": token, "target": str(run_dir)}, indent=2),
    )
    run_id = str(uuid.uuid4())
    client = preparing / "client"
    try:
        shutil.copytree(client_template, client, ignore=copy_ignore)
        overlay_observer(client, stage)
        configure_test_network(client,network_policy)
        recording = configure_sfx_only(client) if sfx_only else None
        if quiet:configure_muted_audio(client)
        write_job(client, run_id, plan, profile, profile_path)

        entry = client / (launcher_path or GAME_EXE)
        if not entry.is_file() or not is_relative_to(entry.resolve(), client.resolve()):
            raise GameTestError(f"configured process entry is not inside the copied client: {entry}")
        immutable, initial = collect_hashes(client)
        source_root = Path(__file__).resolve().parents[1]
        metadata = {
            "schema": SCHEMA_VERSION,
            "run_id": run_id,
            "suite": ('account-bootstrap' if bootstrap_only else 'battleship-acquisition' if battleship_only
                      else 'gift-mail-requests' if gift_requests_only else 'battleship-deck' if deck_only else 'pvp-ai-normal-flow'),
            "status": "prepared",
            "created_utc": utc_now(),
            "source": {
                **git_source_ref(source_root),
                "runner_sha256": sha256(Path(__file__).resolve()),
            },
            "template": str(client_template),
            "stage": str(stage),
            "plan": plan,
            "client": {
                "root": str((run_dir / "client").resolve()),
                "entry": str((run_dir / "client" / (launcher_path or GAME_EXE)).resolve()),
                "game": str((run_dir / "client" / GAME_EXE).resolve()),
                "host_key": HOST_KEY,
                "events": str(
                    (run_dir / "client" / "sdgo" / "gametest" / "events.jsonl").resolve()
                ),
                "stdout": str((run_dir / "receipts" / "client.stdout.log").resolve()),
                "stderr": str((run_dir / "receipts" / "client.stderr.log").resolve()),
                "immutable_hashes": immutable,
                "initial_inputs": initial,
            },
            "execution": None,
        }
        if recording is not None:
            metadata["recording"] = recording
        atomic_write(preparing / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
        rename_prepared_directory(preparing, run_dir)
        (run_dir / ".gametest-preparing.json").unlink()
        return read_json(run_dir / "run.json")
    except GameTestError as error:
        cleanup = _remove_owned_preparation(preparing, token)
        raise GameTestError(f"{error}; {cleanup}") from error
    except BaseException:
        _remove_owned_preparation(preparing, token)
        raise


def validate_metadata(run_dir: Path) -> dict:
    run_dir = run_dir.resolve()
    metadata = read_json(run_dir / "run.json")
    if metadata.get("schema") != SCHEMA_VERSION:
        raise GameTestError("unsupported run.json schema")
    run_id = str(metadata.get("run_id", ""))
    try:
        parsed = uuid.UUID(run_id)
    except ValueError as error:
        raise GameTestError("invalid run_id in run.json") from error
    if run_id != str(parsed):
        raise GameTestError("run_id in run.json must use canonical lowercase UUID form")
    client = metadata.get("client")
    if not isinstance(client, dict):
        raise GameTestError("run.json lacks client metadata")
    root = (run_dir / "client").resolve()
    if Path(client.get("root", "")).resolve() != root:
        raise GameTestError("client root escapes this run directory")
    if Path(client.get("game", "")).resolve() != root / GAME_EXE:
        raise GameTestError("game path does not match this run directory")
    entry = Path(client.get("entry", "")).resolve()
    if not is_relative_to(entry, root):
        raise GameTestError("process entry escapes this run directory")
    if Path(client.get("events", "")).resolve() != root / "sdgo" / "gametest" / "events.jsonl":
        raise GameTestError("native event path does not match this run directory")
    if client.get("host_key") != HOST_KEY:
        raise GameTestError("run.json changes the expected client host")

    plan = metadata.get("plan")
    if not isinstance(plan, dict):
        raise GameTestError("run.json lacks normal-flow plan")
    public_plan = read_public_plan(root)
    for flag in ('bootstrap_only','battleship_only','gift_requests_only','deck_only'):
        if (type(plan.get(flag, False)) is not bool or plan.get(flag, False) != public_plan.get(flag, False)):
            raise GameTestError('run.json cannot change the required bootstrap or battle scope')
    for key, value in public_plan.items():
        if plan.get(key) != value:
            raise GameTestError(f"run.json plan disagrees with run.ini for {key}")
    expected_launcher = plan.get("launcher_path", "")
    expected_entry = (root / (expected_launcher or GAME_EXE)).resolve()
    if entry != expected_entry or not is_relative_to(expected_entry, root):
        raise GameTestError("run.json entry disagrees with its normal-flow plan")
    validate_private_account(root, plan)
    return metadata


def verify_hashes(metadata: dict) -> None:
    root = Path(metadata["client"]["root"]).resolve()
    hashes = metadata["client"].get("immutable_hashes")
    if not isinstance(hashes, dict) or not hashes:
        raise GameTestError("client has no immutable payload hashes")
    for relative, expected in hashes.items():
        if not isinstance(relative, str) or not relative:
            raise GameTestError("invalid immutable payload path in run.json")
        relative_path = Path(relative)
        path = (root / relative_path).resolve()
        if relative_path.is_absolute() or not is_relative_to(path, root):
            raise GameTestError(f"immutable payload path escapes the client capsule: {relative}")
        if (
            not isinstance(expected, str)
            or len(expected) != 64
            or any(character not in "0123456789abcdef" for character in expected)
        ):
            raise GameTestError(f"invalid immutable payload hash for {relative}")
        if not path.is_file() or sha256(path) != expected:
            raise GameTestError(f"immutable client payload changed since preparation: {path}")


def acquire_exclusive_lock(path: Path, label: str) -> tuple[Path, str]:
    token = uuid.uuid4().hex
    path.parent.mkdir(parents=True, exist_ok=True)
    try:
        with path.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump({"token": token, "pid": os.getpid(), "created_utc": utc_now()}, stream)
    except FileExistsError as error:
        raise GameTestError(f"{label} lock already exists and was preserved: {path}") from error
    return path, token


def acquire_lock(run_dir: Path) -> tuple[Path, str]:
    return acquire_exclusive_lock(run_dir / ".run.lock", "run")


def acquire_account_lock(root: Path, plan: dict) -> tuple[Path, str]:
    lock_path = validate_private_account(root, plan)
    return acquire_exclusive_lock(lock_path, "persistent account")


def release_owned_lock(lock: Path, token: str) -> None:
    try:
        if read_json(lock).get("token") == token:
            lock.unlink()
    except (GameTestError, FileNotFoundError):
        pass


class NamedStartupEvent:
    def __init__(self, name: str = STARTUP_EVENT_NAME):
        self.name = name
        self.handle = None

    def __enter__(self):
        if os.name == "nt":
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CreateEventW.argtypes = [
                wintypes.LPVOID,
                wintypes.BOOL,
                wintypes.BOOL,
                wintypes.LPCWSTR,
            ]
            kernel32.CreateEventW.restype = wintypes.HANDLE
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            self.handle = kernel32.CreateEventW(None, False, False, self.name)
            if not self.handle:
                raise OSError(ctypes.get_last_error(), f"cannot create startup event {self.name!r}")
        return self

    def __exit__(self, *_exc):
        if self.handle:
            kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
            kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
            kernel32.CloseHandle.restype = wintypes.BOOL
            kernel32.CloseHandle(self.handle)
            self.handle = None


class ProcessEntry(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.c_size_t),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", wintypes.LONG),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


def process_image_path(pid: int) -> str | None:
    if os.name != "nt":
        return None
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.QueryFullProcessImageNameW.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.LPWSTR,
        ctypes.POINTER(wintypes.DWORD),
    ]
    kernel32.QueryFullProcessImageNameW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    process = kernel32.OpenProcess(0x1000, False, pid)
    if not process:
        return None
    try:
        capacity = wintypes.DWORD(32768)
        buffer = ctypes.create_unicode_buffer(capacity.value)
        if not kernel32.QueryFullProcessImageNameW(process, 0, buffer, ctypes.byref(capacity)):
            return None
        return buffer.value
    finally:
        kernel32.CloseHandle(process)


def process_snapshot() -> list[dict]:
    if os.name != "nt":
        return []
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(ProcessEntry)]
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    snapshot = kernel32.CreateToolhelp32Snapshot(0x00000002, 0)
    invalid = ctypes.c_void_p(-1).value
    if snapshot == invalid:
        return []
    rows = []
    try:
        entry = ProcessEntry()
        entry.dwSize = ctypes.sizeof(entry)
        if kernel32.Process32FirstW(snapshot, ctypes.byref(entry)):
            while True:
                pid = int(entry.th32ProcessID)
                rows.append(
                    {
                        "pid": pid,
                        "parent_pid": int(entry.th32ParentProcessID),
                        "image_path": process_image_path(pid),
                    }
                )
                if not kernel32.Process32NextW(snapshot, ctypes.byref(entry)):
                    break
    finally:
        kernel32.CloseHandle(snapshot)
    return rows


def select_owned_game_process(rows: list[dict], entry_pid: int, expected_game: Path) -> dict | None:
    owned = {entry_pid}
    changed = True
    while changed:
        changed = False
        for row in rows:
            if row.get("parent_pid") in owned and row.get("pid") not in owned:
                owned.add(row["pid"])
                changed = True
    expected = expected_game.resolve()
    candidates = []
    for row in rows:
        image = row.get("image_path")
        if row.get("pid") in owned and image and Path(image).resolve() == expected:
            candidates.append(row)
    return min(candidates, key=lambda row: row["pid"]) if candidates else None


def wait_for_owned_game_process(entry_process, entry_path: Path, expected_game: Path, timeout: float) -> dict | None:
    if entry_path.resolve() == expected_game.resolve():
        return {
            "pid": entry_process.pid,
            "parent_pid": os.getpid(),
            "image_path": str(expected_game.resolve()),
        }
    deadline = time.monotonic() + max(0.0, timeout)
    while True:
        selected = select_owned_game_process(process_snapshot(), entry_process.pid, expected_game)
        if selected:
            return selected
        if time.monotonic() >= deadline:
            return None
        time.sleep(0.1)


def terminate_verified_process(pid: int, expected_path: Path) -> bool:
    current = process_image_path(pid)
    if not current or Path(current).resolve() != expected_path.resolve() or os.name != "nt":
        return False
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel32.OpenProcess.restype = wintypes.HANDLE
    kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    kernel32.TerminateProcess.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel32.CloseHandle.restype = wintypes.BOOL
    process = kernel32.OpenProcess(0x0001 | 0x1000, False, pid)
    if not process:
        return False
    try:
        return bool(kernel32.TerminateProcess(process, 0))
    finally:
        kernel32.CloseHandle(process)


def has_run_end(path: Path) -> bool:
    if not path.is_file():
        return False
    for raw in path.read_bytes().splitlines():
        try:
            event = json.loads(raw.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            continue
        if isinstance(event, dict) and event.get("event") == "run_end":
            return True
    return False


def spawn_entry(metadata: dict, receipt_dir: Path, quiet_runtime=None):
    client = metadata["client"]
    plan = metadata["plan"]
    entry = Path(client["entry"])
    game = Path(client["game"])
    arguments = [str(entry)]
    if entry.resolve() == game.resolve():
        arguments.extend([plan["target_host"], str(plan["login_port"])])
    environment = os.environ.copy()
    environment["SDGO_GAME_TEST_RUN"] = metadata["run_id"]
    environment["SDGO_GAME_TEST_ROLE"] = "client"
    environment["SDGO_GAME_TEST_PLAN"] = str(
        Path(client["root"]) / "sdgo" / "gametest" / "run.ini"
    )
    environment["SDGO_GAME_TEST_ACCOUNT"] = str(
        Path(client["root"]) / "sdgo" / "gametest" / "private" / "account.ini"
    )
    environment["__COMPAT_LAYER"] = "RunAsInvoker"
    stdout = (receipt_dir / "client.stdout.log").open("wb")
    stderr = (receipt_dir / "client.stderr.log").open("wb")
    try:
        launch=quiet_runtime.spawn if quiet_runtime else subprocess.Popen
        process = launch(
            arguments,
            cwd=client["root"],
            env=environment,
            shell=False,
            stdin=subprocess.DEVNULL,
            stdout=stdout,
            stderr=stderr,
            startupinfo=startup_info(),
        )
    except BaseException:
        stdout.close()
        stderr.close()
        raise
    return process, stdout, stderr


def startup_info():
    if os.name != "nt":
        return None
    info = subprocess.STARTUPINFO()
    info.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    info.wShowWindow = 0  # SW_HIDE; quiet runs additionally use a separate desktop.
    return info


def run_prepared(run_dir: Path, timeout: float = 600.0, launch_timeout: float = 30.0) -> dict:
    run_dir = run_dir.resolve()
    metadata = validate_metadata(run_dir)
    verify_hashes(metadata)
    if metadata.get("execution") is not None:
        raise GameTestError("this capsule already has an execution receipt; prepare a fresh run")
    events = Path(metadata["client"]["events"])
    if events.exists():
        raise GameTestError(f"native event file already exists; prepare a fresh run: {events}")

    lock, token = acquire_lock(run_dir)
    account_lock = None
    account_token = None
    receipt_dir = run_dir / "receipts"
    entry_process = None
    game_process = None
    quiet_runtime = None
    handles = []
    started = utc_now()
    try:
        receipt_dir.mkdir(exist_ok=True)
        account_lock, account_token = acquire_account_lock(
            Path(metadata["client"]["root"]), metadata["plan"]
        )
        if metadata['plan'].get('quiet'):
            quiet_runtime=QuietRuntime(metadata,receipt_dir)
            quiet_runtime.start()
        with NamedStartupEvent():
            try:
                entry_process, stdout, stderr = spawn_entry(metadata, receipt_dir,quiet_runtime)
                handles.extend((stdout, stderr))
            except OSError as error:
                metadata["execution"] = {
                    "started_utc": started,
                    "finished_utc": utc_now(),
                    "entry_pid": None,
                    "game_pid": None,
                    "game_image_path": None,
                    "status": "launch_error",
                    "timed_out": False,
                    "error": f"{type(error).__name__}: {error}",
                }
                atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
                return build_report(run_dir)

            game_process = wait_for_owned_game_process(
                entry_process,
                Path(metadata["client"]["entry"]),
                Path(metadata["client"]["game"]),
                launch_timeout,
            )
            if not game_process:
                metadata["execution"] = {
                    "started_utc": started,
                    "finished_utc": utc_now(),
                    "entry_pid": entry_process.pid,
                    "game_pid": None,
                    "game_image_path": None,
                    "status": "launch_timeout",
                    "timed_out": True,
                    "error": "copied GOnline.exe was not observed as the configured entry or its descendant",
                }
                atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
                return build_report(run_dir)

            metadata["execution"] = {
                "started_utc": started,
                "finished_utc": None,
                "entry_pid": entry_process.pid,
                "game_pid": game_process["pid"],
                "game_image_path": str(Path(game_process["image_path"]).resolve()),
                "status": "running",
                "timed_out": False,
                "error": None,
            }
            metadata["status"] = "running"
            atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))

            deadline = time.monotonic() + max(0.0, timeout)
            status = "running"
            while time.monotonic() < deadline:
                if has_run_end(events):
                    status = "completed"
                    break
                if quiet_runtime:
                    proof=quiet_runtime.verify(game_process['pid'])
                    if proof and not metadata['execution'].get('quiet_verified'):
                        metadata['execution']['quiet_verified']=proof
                        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
                if metadata['plan'].get('network_policy')=='direct' and not metadata['execution'].get('network_verified'):
                    proof=verify_direct_peer(metadata,game_process['pid'])
                    if proof:
                        metadata['execution']['network_verified']=proof
                        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
                if process_image_path(game_process["pid"]) is None:
                    status = "game_exited"
                    break
                time.sleep(0.1)
            if status == "running":
                status = "timeout"
            metadata["execution"]["status"] = status
            metadata["execution"]["timed_out"] = status == "timeout"
            metadata["execution"]["finished_utc"] = utc_now()
            metadata["status"] = "executed"
            atomic_write(run_dir / "run.json", json.dumps(metadata, indent=2, sort_keys=True))
    except (GameTestError,OSError) as error:
        if quiet_runtime is None:raise
        execution=metadata.get('execution') or {}
        metadata['execution']=execution
        execution.update(status='launch_error',error=str(error),finished_utc=utc_now())
        metadata['status']='executed'
        atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
    finally:
        if game_process:
            if entry_process and game_process["pid"] == entry_process.pid:
                if entry_process.poll() is None:
                    entry_process.kill()
                    try:
                        entry_process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        pass
            else:
                terminate_verified_process(game_process["pid"], Path(metadata["client"]["game"]))
        if entry_process and entry_process.poll() is None:
            entry_process.kill()
            try:
                entry_process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass
        for handle in handles:
            handle.close()
        if quiet_runtime:
            metadata.setdefault('execution',{})['quiet_cleanup']=quiet_runtime.close()
            atomic_write(run_dir/'run.json',json.dumps(metadata,indent=2,sort_keys=True))
        if account_lock is not None and account_token is not None:
            release_owned_lock(account_lock, account_token)
        release_owned_lock(lock, token)
    return build_report(run_dir)


def evaluate_events(
    path: Path,
    run_id: str,
    expected_pid: int | None,
    expected_image: Path,
    required_cases: tuple[str, ...],
) -> dict:
    errors: list[str] = []
    pending: list[str] = []
    events: list[dict] = []
    if not path.is_file():
        pending.append(f"native event log has not been created: {path}")
    else:
        for line_number, raw in enumerate(path.read_bytes().splitlines(), 1):
            try:
                event = json.loads(raw.decode("utf-8"))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                errors.append(f"line {line_number}: malformed or truncated JSON: {error}")
                continue
            if not isinstance(event, dict):
                errors.append(f"line {line_number}: event is not an object")
                continue
            required_fields = (
                "run_id",
                "role",
                "event",
                "case",
                "status",
                "message",
                "host_key",
                "pid",
                "image_path",
                "duration_ms",
            )
            missing = [field for field in required_fields if field not in event]
            if missing:
                errors.append(f"line {line_number}: missing fields {', '.join(missing)}")
                continue
            if event["run_id"] != run_id or event["role"] != "client":
                errors.append(f"line {line_number}: foreign run_id or role")
                continue
            if (
                not isinstance(event["event"], str)
                or not isinstance(event["case"], str)
                or not isinstance(event["status"], str)
                or not isinstance(event["message"], str)
                or event["host_key"] != HOST_KEY
                or not isinstance(event["pid"], int)
                or event["pid"] <= 0
                or not isinstance(event["image_path"], str)
                or not isinstance(event["duration_ms"], (int, float))
                or event["duration_ms"] < 0
            ):
                errors.append(f"line {line_number}: invalid native event fields")
                continue
            try:
                image = Path(event["image_path"]).resolve()
            except OSError:
                errors.append(f"line {line_number}: invalid native image path")
                continue
            if image != expected_image.resolve():
                errors.append(f"line {line_number}: event came from a different executable: {image}")
                continue
            if expected_pid is not None and event["pid"] != expected_pid:
                errors.append(
                    f"line {line_number}: native pid {event['pid']} does not match owned pid {expected_pid}"
                )
                continue
            events.append(event)

    allowed = {
        "run_begin": {"running"},
        "observation": {"info"},
        "case_begin": {"running"},
        "assert": {"pass", "fail"},
        "case_end": {"pass", "fail", "skip"},
        "run_end": {"pass", "fail"},
    }
    for event in events:
        if event["event"] not in allowed or event["status"] not in allowed[event["event"]]:
            errors.append(f"invalid {event['event']!r} event status {event['status']!r}")
    if events:
        identity = {(event["pid"], event["image_path"], event["host_key"]) for event in events}
        if len(identity) != 1:
            errors.append("native process identity changed during the run")

    begins = [event for event in events if event["event"] == "run_begin"]
    ends = [event for event in events if event["event"] == "run_end"]
    if len(begins) > 1:
        errors.append(f"expected at most one run_begin, found {len(begins)}")
    if len(ends) > 1:
        errors.append(f"expected at most one run_end, found {len(ends)}")
    if events and events[0]["event"] != "run_begin":
        errors.append("run_begin is not the first native event")
    if ends and events[-1]["event"] != "run_end":
        errors.append("run_end is not the last native event")

    cases: dict[str, dict] = {}
    order: list[str] = []
    for event in events:
        if event["event"] == "assert" and event["status"] == "fail":
            errors.append(f"failed assertion: {event['case'] or event['message'] or 'unnamed'}")
        if event["event"] != "case_end":
            continue
        case_id = event["case"]
        if not case_id:
            errors.append("case_end has no case id")
            continue
        if case_id in cases:
            errors.append(f"duplicate case_end: {case_id}")
            continue
        cases[case_id] = {
            "status": event["status"],
            "message": event["message"],
            "duration_ms": event["duration_ms"],
        }
        order.append(case_id)
        if event["status"] != "pass":
            errors.append(f"case {case_id} status is {event['status']!r}")

    observed_server = None
    observed_channel = None
    selection_event_count = 0
    for event in events:
        if "server_id" not in event and "channel_id" not in event:
            continue
        invalid_selection_fields = []
        for field in ("server_id", "channel_id"):
            if field not in event:
                continue
            value = event[field]
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                invalid_selection_fields.append(field)
        if invalid_selection_fields:
            errors.append(
                "invalid native server/channel observation fields: "
                + ", ".join(invalid_selection_fields)
            )
            continue
        selection_event_count += 1
        if "server_id" in event:
            observed_server = event["server_id"]
        if "channel_id" in event:
            observed_channel = event["channel_id"]

    missing_cases = [case_id for case_id in required_cases if case_id not in cases]
    if missing_cases:
        pending.append(f"normal flow is incomplete; missing cases: {', '.join(missing_cases)}")
    present_required = [case_id for case_id in order if case_id in required_cases]
    expected_present = [case_id for case_id in required_cases if case_id in cases]
    if present_required != expected_present:
        errors.append("normal-flow cases completed out of order")
    if not begins:
        pending.append("native run_begin has not been observed")
    if not ends:
        pending.append("native run_end has not been observed")
    elif ends[0]["status"] != "pass":
        errors.append(f"run_end status is {ends[0]['status']!r}")
    elif missing_cases:
        errors.append("native run_end claimed pass before the required normal flow completed")

    status = "fail" if errors else ("pending" if pending else "pass")
    if observed_server is not None and observed_channel is not None:
        selection_status = "observed"
    elif observed_server is not None or observed_channel is not None:
        selection_status = "partial"
    else:
        selection_status = "unobserved"
    return {
        "status": status,
        "event_count": len(events),
        "cases": cases,
        "required_cases": list(required_cases),
        "server_channel_selection": {
            "policy": "native_automatic",
            "source": "native_events",
            "status": selection_status,
            "observed_server_id": observed_server,
            "observed_channel_id": observed_channel,
            "observation_event_count": selection_event_count,
        },
        "errors": errors,
        "pending": pending,
        "path": str(path),
    }


def junit_report(report: dict) -> str:
    root = ET.Element("testsuites")
    native = report["layers"]["native"]
    suite = ET.SubElement(root, "testsuite", name="sdgo-gametest-normal-client")
    required_cases = tuple(native["required_cases"])
    failures = 0
    skipped = 0
    for case_id in required_cases:
        test = ET.SubElement(suite, "testcase", name=case_id, classname="client.normal-flow")
        case = native["cases"].get(case_id)
        if case is None:
            skipped += 1
            ET.SubElement(test, "skipped", message="normal flow has not reached this case")
        elif case["status"] != "pass":
            failures += 1
            message = case.get("message") or f"status={case['status']!r}"
            ET.SubElement(test, "failure", message=message).text = message
        elif isinstance(case.get("duration_ms"), (int, float)):
            test.set("time", f"{case['duration_ms'] / 1000.0:.6f}")
    extra_tests = 0
    if report.get("status") == "fail" and failures == 0:
        messages = []
        for layer_name in ("source", "launch", "native"):
            layer = report["layers"].get(layer_name, {})
            for error in layer.get("errors", []):
                messages.append(f"{layer_name}: {error}")
        message = "; ".join(messages) or "GameTest failed outside an individual native case"
        test = ET.SubElement(
            suite,
            "testcase",
            name="framework.infrastructure",
            classname="client.infrastructure",
        )
        ET.SubElement(test, "failure", message=message).text = message
        failures += 1
        extra_tests = 1
    suite.set("tests", str(len(required_cases) + extra_tests))
    suite.set("failures", str(failures))
    suite.set("errors", "0")
    suite.set("skipped", str(skipped))
    ET.indent(root, space="  ")
    return ET.tostring(root, encoding="unicode", xml_declaration=True)


def build_report(run_dir: Path) -> dict:
    run_dir = run_dir.resolve()
    metadata = validate_metadata(run_dir)
    source_errors = []
    try:
        verify_hashes(metadata)
    except GameTestError as error:
        source_errors.append(str(error))
    execution = metadata.get("execution")
    launch_errors = []
    launch_pending = []
    if execution is None:
        launch_pending.append("normal copied client has not been launched")
    elif execution.get("status") in {"launch_error", "launch_timeout"}:
        launch_errors.append(execution.get("error") or execution["status"])
    elif execution.get("game_pid") is None or Path(execution.get("game_image_path", "")).resolve() != Path(
        metadata["client"]["game"]
    ).resolve():
        launch_errors.append("execution is not bound to the copied GOnline.exe")
    else:
        execution_status = execution.get("status")
        if execution_status == "running":
            launch_pending.append("normal copied client is still running")
        elif execution_status != "completed":
            launch_errors.append(
                execution.get("error")
                or {
                    "game_exited": "normal copied client exited before GameTest completed",
                    "timeout": "normal copied client timed out before GameTest completed",
                }.get(execution_status, f"unexpected execution status: {execution_status!r}")
            )
    launch_status = "fail" if launch_errors else ("pending" if launch_pending else "pass")
    if execution is not None and execution.get('status')=='completed' and metadata['plan'].get('network_policy')=='direct':
        proof=execution.get('network_verified',{})
        if (proof.get('verified') is not True or proof.get('pid')!=execution.get('game_pid') or
            proof.get('remote_ipv4')!=metadata['plan']['target_host'] or proof.get('remote_port')!=metadata['plan']['login_port']):
            launch_errors.append('the actual native login TCP peer was not verified against the requested endpoint')
            launch_status='fail'
    if execution is not None and execution.get('status')=='completed' and metadata['plan'].get('quiet'):
        proof=execution.get('quiet_verified',{})
        cleanup=execution.get('quiet_cleanup',{})
        if (proof.get('verified') is not True or proof.get('foreground_safe') is not True or
            proof.get('pid')!=execution.get('game_pid') or proof.get('audio_muted') is not True or
            proof.get('firewall_verified') is not True or cleanup.get('status')!='closed' or
            cleanup.get('cleanup',{}).get('status')!='removed'):
            launch_errors.append('quiet desktop, muted audio, scoped firewall and cleanup evidence are incomplete')
            launch_status='fail'

    required_cases = required_flow_cases(metadata["plan"]["mode_sequence"], metadata['plan'].get('bootstrap_only', False),
        metadata['plan'].get('battleship_only',False),metadata['plan'].get('gift_requests_only',False),metadata['plan'].get('deck_only',False))
    native = evaluate_events(
        Path(metadata["client"]["events"]),
        metadata["run_id"],
        execution.get("game_pid") if execution else None,
        Path(metadata["client"]["game"]),
        required_cases,
    )
    if (
        execution is not None
        and execution.get("status") == "completed"
        and native["status"] == "pending"
    ):
        launch_errors.append("execution completed without the required native GameTest evidence")
        launch_status = "fail"
    report = {
        "schema": SCHEMA_VERSION,
        "run_id": metadata["run_id"],
        "suite": metadata["suite"],
        "generated_utc": utc_now(),
        "layers": {
            "source": {
                "status": "bound" if not source_errors else "fail",
                "source_ref": metadata.get("source"),
                "errors": source_errors,
            },
            "launch": {
                "status": launch_status,
                "errors": launch_errors,
                "pending": launch_pending,
                "execution": execution,
                "stdout": metadata["client"]["stdout"],
                "stderr": metadata["client"]["stderr"],
            },
            "native": native,
        },
    }
    statuses = (report["layers"]["source"]["status"], launch_status, native["status"])
    report["status"] = (
        "fail"
        if "fail" in statuses
        else ("pass" if all(value in {"bound", "pass"} for value in statuses) else "pending")
    )
    receipt_dir = run_dir / "receipts"
    receipt_dir.mkdir(exist_ok=True)
    atomic_write(receipt_dir / "report.json", json.dumps(report, indent=2, sort_keys=True))
    atomic_write(receipt_dir / "junit.xml", junit_report(report))
    return report


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subcommands = parser.add_subparsers(dest="command", required=True)

    prepare_parser = subcommands.add_parser("prepare", help="copy a normal client and add GameTest")
    prepare_parser.add_argument("--client-template", type=Path, required=True)
    prepare_parser.add_argument(
        "--stage",
        type=Path,
        required=True,
        help="built client stage; overlays the observer and a staged production match_ai.dll when present",
    )
    prepare_parser.add_argument("--run-dir", type=Path, required=True)
    prepare_parser.add_argument("--target-host", default="127.0.0.1")
    prepare_parser.add_argument("--login-port", type=int, default=5001)
    prepare_parser.add_argument(
        "--target-server",
        default="1",
        help=(
            "deprecated compatibility value, recorded only; the normal client uses its "
            "native automatic server selection"
        ),
    )
    prepare_parser.add_argument(
        "--target-channel",
        default="1",
        help=(
            "deprecated compatibility value, recorded only; the normal client uses its "
            "native automatic channel selection"
        ),
    )
    prepare_parser.add_argument("--mode-sequence", default="1")
    prepare_parser.add_argument("--retry-count", type=int, choices=(1, 2))
    prepare_parser.add_argument("--expected-ai", type=int, default=3)
    prepare_parser.add_argument("--sfx-only", action="store_true",
                                help="mute BGM and enable effects only in the isolated client")
    prepare_parser.add_argument('--bootstrap-only', action='store_true',
                                help='test real login and onboarding only; does not claim PvP acceptance')
    prepare_parser.add_argument('--battleship-only', action='store_true',help='acquire the initial battleships through native tutorial business')
    prepare_parser.add_argument('--gift-requests-only', action='store_true',help='send configured native mail gift requests; server processing is verified separately')
    prepare_parser.add_argument('--deck-only', action='store_true',help='save and select a native battleship deck using four genuinely owned unit UIDs from the private account profile')
    prepare_parser.add_argument('--network-policy',choices=('direct','preserve'),default='direct',
                                help='direct disables inherited endpoint routes only in the test copy; preserve explicitly tests owner routes')
    prepare_parser.add_argument('--visible',action='store_true',help='explicit developer opt-in to a visible client; default tests use an isolated desktop and muted audio')
    prepare_parser.add_argument(
        "--expect-first-fail",
        action=argparse.BooleanOptionalAction,
        default=None,
    )
    prepare_parser.add_argument(
        "--account-profile",
        type=Path,
        help=r"persistent account profile; default %%LOCALAPPDATA%%\SDGO-GameTest\account.ini",
    )
    prepare_parser.add_argument(
        "--create-test-account",
        action="store_true",
        help="explicit one-time authorization to create a missing persistent test account profile",
    )
    prepare_parser.add_argument(
        "--launcher-path",
        help="optional path inside the copied client; default starts GOnline.exe directly",
    )

    run_parser = subcommands.add_parser("run", help="run the copied normal client")
    run_parser.add_argument("--run-dir", type=Path, required=True)
    run_parser.add_argument("--timeout", type=float, default=600.0)
    run_parser.add_argument("--launch-timeout", type=float, default=30.0)

    report_parser = subcommands.add_parser("report", help="rebuild receipts from native evidence")
    report_parser.add_argument("--run-dir", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        if args.command == "prepare":
            result = prepare(
                args.client_template,
                args.stage,
                args.run_dir,
                target_host=args.target_host,
                login_port=args.login_port,
                target_server=args.target_server,
                target_channel=args.target_channel,
                mode_sequence=args.mode_sequence,
                retry_count=args.retry_count,
                expect_first_fail=args.expect_first_fail,
                expected_ai=args.expected_ai,
                launcher_path=args.launcher_path,
                account_profile=args.account_profile,
                create_test_account=args.create_test_account,
                sfx_only=args.sfx_only,
                bootstrap_only=args.bootstrap_only,
                battleship_only=args.battleship_only,
                gift_requests_only=args.gift_requests_only,
                deck_only=args.deck_only,
                network_policy=args.network_policy,
                quiet=not args.visible,
            )
        elif args.command == "run":
            result = run_prepared(args.run_dir, args.timeout, args.launch_timeout)
        else:
            result = build_report(args.run_dir)
        print(json.dumps(result, indent=2, sort_keys=True))
        return 0 if args.command == "prepare" or result.get("status") == "pass" else 1
    except GameTestError as error:
        print(f"game_test: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
