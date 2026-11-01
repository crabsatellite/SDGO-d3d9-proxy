"""Compatibility import; the authoritative implementation belongs to Mod Manager."""
from pathlib import Path
import sys
ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))
from manager.engines.capsules import capsule_catalog as implementation
sys.modules[__name__] = implementation
