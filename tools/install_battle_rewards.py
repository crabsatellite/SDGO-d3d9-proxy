"""Plan, install, update or disable the Battle Rewards server mod."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards.deployment import (
    DeploymentApplyError,
    DeploymentError,
    apply_plan,
    build_plan,
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package-root", type=Path)
    parser.add_argument("--battle-dir", type=Path, required=True)
    parser.add_argument("--workspace", type=Path, required=True)
    parser.add_argument("--server-ini", type=Path)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="Perform the reviewed plan. Without this flag nothing is written.",
    )
    parser.add_argument(
        "--uninstall",
        action="store_true",
        help="Plan/apply a scoped disable-and-archive operation.",
    )
    args = parser.parse_args()
    try:
        plan = build_plan(
            battle_dir=args.battle_dir,
            workspace=args.workspace,
            package_root=args.package_root,
            server_ini=args.server_ini,
            uninstall=args.uninstall,
        )
        if args.apply:
            result = apply_plan(plan)
        else:
            result = plan.public()
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return 0
    except DeploymentApplyError as error:
        print(
            json.dumps(
                {
                    "schema_version": 1,
                    "mode": "error",
                    "error": str(error),
                    "receipt": str(error.receipt),
                    "writes_performed": error.writes_performed,
                },
                ensure_ascii=False,
                indent=2,
            ),
            file=sys.stderr,
        )
        return 1
    except (DeploymentError, OSError, ValueError) as error:
        print(
            json.dumps(
                {
                    "schema_version": 1,
                    "mode": "error",
                    "error": str(error),
                    "writes_performed": False,
                },
                ensure_ascii=False,
                indent=2,
            ),
            file=sys.stderr,
        )
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
