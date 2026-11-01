from pathlib import Path
import subprocess
import sys
import tempfile
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
from owner.rewards.boss_catalog import publish

if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("usage: reward_boss_native_probe.py TEST_EXE")
    with tempfile.TemporaryDirectory() as directory:
        registry = Path(directory) / "owner runtime" / "reward-bosses.txt"
        publish(registry, [(11, 30), (10, 20)])
        subprocess.run([sys.argv[1], str(registry)], check=True)
