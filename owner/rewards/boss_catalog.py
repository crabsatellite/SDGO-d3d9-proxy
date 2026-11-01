"""Read the owner's native mission/Boss mapping and export a portable registry.

The native MissionRankBossData table supplies identities. BossFlag and model IDs
are deliberately not treated as boss identity; they are ambiguous in this game.
This module only SELECTs and never changes game data or wallet balances.
"""
from __future__ import annotations
import argparse
import configparser
import hashlib
import json
import os
from pathlib import Path
import uuid


def normalize(rows):
    pairs = set()
    for row in rows:
        if not isinstance(row, (list, tuple)) or len(row) != 2:
            raise ValueError("Boss registry requires MissionID/NpcDataID pairs")
        mission, npc = row
        if any(type(value) is not int or not 0 < value <= 2147483647 for value in row):
            raise ValueError("Boss registry identities must be positive native integers")
        pairs.add((mission, npc))
    if len(pairs) > 100000:
        raise ValueError("Boss registry exceeds the supported native bound")
    return sorted(pairs)


def encode(rows):
    pairs = normalize(rows)
    body = "".join(f"{mission},{npc}\n" for mission, npc in pairs).encode("ascii")
    digest = hashlib.sha256(body).hexdigest()
    return b"SDGO_BOSS_REGISTRY_V1 " + digest.encode("ascii") + b"\n" + body


def read_native(connection):
    cursor = connection.cursor()
    rows = cursor.execute("SELECT MissionID,BossID_1,BossID_2,BossID_3,BossID_4,BossID_5 FROM dbo.tbl_MissionRankBossData").fetchall()
    pairs = []
    for row in rows:
        mission = int(row[0])
        for value in row[1:]:
            if value is not None and int(value):
                pairs.append((mission, int(value)))
    result = normalize(pairs)
    known = {int(row[0]) for row in cursor.execute("SELECT NpcDataID FROM dbo.tbl_NpcData").fetchall()}
    if any(npc not in known for _, npc in result):
        raise ValueError("Native Boss registry references missing NpcData identities")
    return result


def publish(path, rows):
    path = Path(path)
    data = encode(rows)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name("." + path.name + "." + uuid.uuid4().hex + ".tmp")
    try:
        with temporary.open("xb") as stream:
            stream.write(data); stream.flush(); os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if temporary.exists():
            temporary.unlink()
    return data.split(b"\n", 1)[0].split(b" ", 1)[1].decode("ascii")


def connect_game(server_ini):
    import pyodbc
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(server_ini, encoding="utf-8-sig")
    section = next((name for name in parser.sections() if name.lower().replace(" ", "") == "dbgame"), None)
    if section is None:
        raise ValueError("Selected owner INI has no DB Game configuration")
    db = parser[section]
    escape = lambda value: "{" + str(value).replace("}", "}}") + "}"
    drivers = [name for name in pyodbc.drivers() if "SQL Server" in name]
    if not drivers:
        raise ValueError("No SQL Server ODBC driver is installed")
    text = (f"DRIVER={escape(drivers[-1])};SERVER={escape(db['DBServerName'])};"
            f"DATABASE={escape(db['DBName'])};UID={escape(db['ID'])};PWD={escape(db['PWD'])};"
            "APP=SDGORewardBossRegistryRead;")
    try:
        connection = pyodbc.connect(text, timeout=5, autocommit=False)
        connection.timeout = 15
        return connection
    except pyodbc.Error as error:
        raise ValueError("Owner game DB connection failed; SQLSTATE=" + str(error.args[0])) from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ini", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    connection = connect_game(args.server_ini)
    try:
        pairs = read_native(connection)
        revision = publish(args.out, pairs)
    finally:
        connection.rollback(); connection.close()
    print(json.dumps({"read_only_database": True, "registry_pairs": len(pairs), "revision": revision,
                      "output": str(args.out.resolve())}))


if __name__ == "__main__": main()
