from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import random
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from owner.rewards import model


def decimal(micro: int) -> str:
    whole, fraction = divmod(micro, 1_000_000)
    if not fraction:
        return str(whole)
    return f"{whole}.{fraction:06d}".rstrip("0")


def compile_test(executable: Path) -> None:
    compiler = shutil.which("clang++") or shutil.which("g++")
    if not compiler:
        raise RuntimeError("clang++ or g++ is required for the standalone parity test")
    executable.parent.mkdir(parents=True, exist_ok=True)
    command = [
        compiler,
        "-std=c++17",
        "-O2",
        "-Wall",
        "-Wextra",
        "-pedantic",
        "-I",
        str(ROOT / "include"),
        "-I",
        str(ROOT),
        str(ROOT / "tests" / "battle_reward_policy_test.cpp"),
        "-o",
        str(executable),
    ]
    subprocess.run(command, cwd=ROOT, check=True)


def rate_lines(prefix: str, value: dict) -> list[str]:
    return [f"{prefix}Gb={value['gb']}", f"{prefix}Mb={value['mb']}"]


def policy_ini(policy: dict) -> str:
    pvp = policy["pvp"]
    pve = policy["pve"]
    lines = [
        "[meta]",
        "SchemaVersion=1",
        f"PolicyRevision={model.revision(policy)}",
        "[timing]",
        f"FullRewardSeconds={policy['full_reward_seconds']}",
        f"MinimumActiveSeconds={policy['minimum_active_seconds']}",
        "[multipliers]",
        f"Base={policy['base_multiplier']}",
        f"ExtraHuman={policy['extra_human_multiplier']}",
        "[pvp]",
    ]
    for name, value in (
        ("BaseWin", pvp["base"]["win"]),
        ("BaseLose", pvp["base"]["lose"]),
        ("BaseDraw", pvp["base"]["draw"]),
        ("Kill", pvp["kill"]),
        ("Assist", pvp["assist"]),
        ("DamagePer1000", pvp["damage_per_1000"]),
        ("Objective", pvp["objective"]),
        ("ContributionCap", pvp["contribution_cap"]),
        ("Mvp", pvp["mvp"]),
    ):
        lines.extend(rate_lines(name, value))
    lines.append("[pve]")
    for name, value in (
        ("BaseClear", pve["base"]["clear"]),
        ("BaseFail", pve["base"]["fail"]),
        ("Minion", pve["minion"]),
        ("BossCredit", pve["boss_credit"]),
        ("DamagePer1000", pve["damage_per_1000"]),
        ("Objective", pve["objective"]),
        ("ContributionCap", pve["contribution_cap"]),
        ("TotalCap1x", pve["total_cap_1x"]),
    ):
        lines.extend(rate_lines(name, value))
    lines.extend(
        [
            f"DifficultyNormal={pve['difficulty']['normal']}",
            f"DifficultyHard={pve['difficulty']['hard']}",
            f"DifficultyExpert={pve['difficulty']['expert']}",
        ]
    )
    return "\r\n".join(lines) + "\r\n"


def random_policy(rng: random.Random) -> dict:
    policy = copy.deepcopy(model.defaults())
    policy["base_multiplier"] = decimal(rng.randint(0, 40_000_000))
    policy["full_reward_seconds"] = rng.randint(30, 600)
    policy["minimum_active_seconds"] = rng.randint(
        1, policy["full_reward_seconds"]
    )

    def money(max_gb: int, max_mb: int) -> dict:
        return {
            "gb": decimal(rng.randint(0, max_gb * 1_000_000)),
            "mb": decimal(rng.randint(0, max_mb * 1_000_000)),
        }

    for outcome in ("win", "lose", "draw"):
        policy["pvp"]["base"][outcome] = money(800, 8)
    for key, bounds in (
        ("kill", (100, 2)),
        ("assist", (50, 1)),
        ("damage_per_1000", (30, 1)),
        ("objective", (80, 2)),
        ("contribution_cap", (1000, 10)),
        ("mvp", (500, 15)),
    ):
        policy["pvp"][key] = money(*bounds)
    for outcome in ("clear", "fail"):
        policy["pve"]["base"][outcome] = money(250, 3)
    for key, bounds in (
        ("minion", (5, 1)),
        ("boss_credit", (100, 3)),
        ("damage_per_1000", (5, 1)),
        ("objective", (30, 1)),
        ("contribution_cap", (300, 5)),
        ("total_cap_1x", (600, 8)),
    ):
        policy["pve"][key] = money(*bounds)
    for difficulty in ("normal", "hard", "expert"):
        policy["pve"]["difficulty"][difficulty] = decimal(
            rng.randint(1, 3_000_000)
        )
    return model.validate(policy)


def make_case(rng: random.Random, case_id: str, policy: dict) -> tuple[str, dict, dict]:
    mode = rng.choice(("pvp", "pve"))
    duration = rng.randint(0, 1200)
    active = rng.randint(0, duration)
    bosses = rng.randint(0, 5)
    credit_micro = rng.randint(0, bosses * 1_000_000)
    context = {
        "mode": mode,
        "outcome": rng.choice(("win", "lose", "draw"))
        if mode == "pvp"
        else rng.choice(("clear", "fail")),
        "difficulty": rng.choice(("normal", "hard", "expert")),
        "human_count": rng.randint(1, 12),
        "event_multiplier": str(rng.choice((1, 2))),
        "duration_seconds": duration,
        "bosses_defeated": bosses,
    }
    stats = {
        "kind": rng.choice(("human", "human", "human", "ai", "observer")),
        "active_seconds": active,
        "kills": rng.randint(0, 255),
        "assists": rng.randint(0, 255),
        "minion_kills": rng.randint(0, 10000),
        "damage": rng.randint(0, 100_000_000),
        "objectives": rng.randint(0, 1000),
        "boss_credit": decimal(credit_micro),
        "is_mvp": bool(rng.getrandbits(1)),
    }
    fields = [
        case_id,
        mode,
        context["outcome"],
        context["difficulty"],
        str(context["human_count"]),
        context["event_multiplier"],
        str(duration),
        str(bosses),
        stats["kind"],
        str(active),
        str(stats["kills"]),
        str(stats["assists"]),
        str(stats["minion_kills"]),
        str(stats["damage"]),
        str(stats["objectives"]),
        str(credit_micro),
        "1" if stats["is_mvp"] else "0",
        str(2_147_483_647),
        str(2_147_483_647),
    ]
    return "\t".join(fields), context, stats


def edge_cases(policy: dict) -> list[tuple[str, dict, dict]]:
    return [
        (
            "edge-third",
            {
                "mode": "pvp",
                "outcome": "win",
                "difficulty": "normal",
                "human_count": 1,
                "event_multiplier": "1",
                "duration_seconds": 60,
                "bosses_defeated": 0,
            },
            {
                "kind": "human",
                "active_seconds": 60,
                "kills": 255,
                "assists": 0,
                "minion_kills": 0,
                "damage": 100000,
                "objectives": 0,
                "boss_credit": "0",
                "is_mvp": False,
            },
        ),
        (
            "edge-twelve",
            {
                "mode": "pvp",
                "outcome": "win",
                "difficulty": "normal",
                "human_count": 12,
                "event_multiplier": "2",
                "duration_seconds": policy["full_reward_seconds"],
                "bosses_defeated": 0,
            },
            {
                "kind": "human",
                "active_seconds": policy["full_reward_seconds"],
                "kills": 255,
                "assists": 255,
                "minion_kills": 0,
                "damage": 100_000_000,
                "objectives": 1000,
                "boss_credit": "0",
                "is_mvp": True,
            },
        ),
    ]


def encode_case(case_id: str, context: dict, stats: dict) -> str:
    credit = model.number(stats.get("boss_credit", "0"), 100)
    credit_micro = credit * 1_000_000
    assert credit_micro.denominator == 1
    return "\t".join(
        [
            case_id,
            context["mode"],
            context["outcome"],
            context.get("difficulty", "normal"),
            str(context["human_count"]),
            str(context["event_multiplier"]),
            str(context["duration_seconds"]),
            str(context.get("bosses_defeated", 0)),
            stats.get("kind", "human"),
            str(stats.get("active_seconds", 0)),
            str(stats.get("kills", 0)),
            str(stats.get("assists", 0)),
            str(stats.get("minion_kills", 0)),
            str(stats.get("damage", 0)),
            str(stats.get("objectives", 0)),
            str(credit_micro.numerator),
            "1" if stats.get("is_mvp", False) else "0",
            str(2_147_483_647),
            str(2_147_483_647),
        ]
    )


def compare_policy(executable: Path, policy: dict, rng: random.Random, index: int) -> int:
    cases: list[tuple[str, dict, dict]] = []
    if index == 0:
        cases.extend(edge_cases(policy))
    rows: list[str] = []
    expected: dict[str, dict] = {}
    for case_id, context, stats in cases:
        rows.append(encode_case(case_id, context, stats))
        expected[case_id] = model.preview(policy, context, stats)
    for number in range(100):
        row, context, stats = make_case(rng, f"p{index}-{number}", policy)
        case_id = row.split("\t", 1)[0]
        rows.append(row)
        expected[case_id] = model.preview(policy, context, stats)

    with tempfile.TemporaryDirectory() as directory:
        ini = Path(directory) / "reward-policy.ini"
        ini.write_text(policy_ini(policy), encoding="ascii", newline="")
        process = subprocess.run(
            [str(executable), "--batch", str(ini)],
            input="\n".join(rows) + "\n",
            text=True,
            capture_output=True,
            cwd=ROOT,
            check=True,
        )
    returned: dict[str, list[str]] = {}
    for line in process.stdout.splitlines():
        fields = line.split("\t")
        returned[fields[0]] = fields[1:]
    if set(returned) != set(expected):
        raise AssertionError(
            f"case identity mismatch: missing={set(expected) - set(returned)} "
            f"extra={set(returned) - set(expected)}"
        )
    for case_id, python_result in expected.items():
        fields = returned[case_id]
        if fields[0] != "OK":
            raise AssertionError(f"{case_id}: C++ rejected valid case: {fields}")
        actual = {
            "gb": int(fields[1]),
            "mb": int(fields[2]),
            "eligible": fields[3] == "1",
        }
        wanted = {
            "gb": python_result["amount"]["gb"],
            "mb": python_result["amount"]["mb"],
            "eligible": python_result["eligible"],
        }
        if actual != wanted:
            raise AssertionError(
                f"{case_id}: C++ {actual} != Python {wanted}\n"
                f"context={json.dumps(cases, ensure_ascii=False)}"
            )
    return len(expected)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--exe",
        type=Path,
        default=ROOT / "build" / "reward-policy-test" / "battle_reward_policy_test.exe",
    )
    parser.add_argument("--no-build", action="store_true")
    args = parser.parse_args()
    executable = args.exe.resolve()
    if not args.no_build:
        compile_test(executable)
    subprocess.run([str(executable)], cwd=ROOT, check=True)

    rng = random.Random(0x5344474F)
    policies = [model.defaults(), *(random_policy(rng) for _ in range(7))]
    checked = sum(
        compare_policy(executable, policy, rng, index)
        for index, policy in enumerate(policies)
    )
    print(f"C++/Python exact parity: {checked} cases across {len(policies)} policies")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
