"""Run the native battle-reward MB ledger worker using the owner's DB INI."""
import argparse
import configparser
import json
from pathlib import Path
import sys
import time
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards.sql_repository import RewardWalletRepository
from owner.rewards.worker import WalletWorker


def connect(ini):
    import pyodbc
    parser = configparser.ConfigParser(interpolation=None)
    parser.read(ini, encoding="utf-8-sig")
    name = next((s for s in parser.sections() if s.lower().replace(" ", "") == "dbstorebilling"), None)
    if name is None:
        raise ValueError("Owner INI has no DB StoreBilling section")
    db = parser[name]
    escape = lambda value: "{" + str(value).replace("}", "}}") + "}"
    drivers = [d for d in pyodbc.drivers() if "SQL Server" in d]
    if not drivers:
        raise ValueError("No SQL Server ODBC driver is installed")
    connection = (f"DRIVER={escape(drivers[-1])};SERVER={escape(db['DBServerName'])};"
                  f"DATABASE={escape(db['DBName'])};UID={escape(db['ID'])};PWD={escape(db['PWD'])};APP=SDGORewardMbWorker;")
    try:
        result = pyodbc.connect(connection, timeout=5, autocommit=False)
        result.timeout = 15
        return result
    except pyodbc.Error as error:
        raise ValueError("Wallet connection failed; SQLSTATE=" + str(error.args[0])) from None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server-ini", type=Path, required=True)
    parser.add_argument("--runtime", type=Path, required=True)
    parser.add_argument("--install-ledger", action="store_true", help="Install only the versioned reward ledger; leaves native wallet procedures intact")
    parser.add_argument("--once", action="store_true")
    args = parser.parse_args()
    connection = connect(args.server_ini)
    worker = None
    try:
        repository = RewardWalletRepository(connection)
        if args.install_ledger:
            repository.install()
        worker = WalletWorker(args.runtime, repository)
        print(json.dumps({"worker": "battle_reward_mb", "runtime": str(worker.root), "scope_id": str(worker.scope), "gb_delivery": "native_only"}), flush=True)
        while True:
            healthy = worker.poll()
            if args.once:
                if not healthy:
                    raise SystemExit(1)
                break
            time.sleep(1)
    finally:
        if worker is not None:
            worker.last_error = "wallet_worker_stopped"; worker.heartbeat(False)
        connection.close()


if __name__ == "__main__": main()
