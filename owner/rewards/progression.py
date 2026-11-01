"""Owner-configured, once-per-account milestones from native settlement jobs."""
from __future__ import annotations

import copy
from datetime import datetime, timedelta, timezone

CHINA = timezone(timedelta(hours=8))


def defaults():
    return {"enabled": False, "minimum_active_seconds": 180,
            "daily_first_mb": 3000, "daily_matches": 3, "daily_mb": 27000,
            "weekly_matches": 12, "weekly_mb": 15000}


def validate(value):
    if not isinstance(value, dict) or set(value) != set(defaults()):
        raise ValueError("日周奖励配置字段不完整")
    if type(value["enabled"]) is not bool:
        raise ValueError("日周奖励开关须为布尔值")
    bounds = {"minimum_active_seconds": (180, 3600), "daily_matches": (2, 20),
              "weekly_matches": (3, 100), "daily_first_mb": (0, 100000),
              "daily_mb": (0, 100000), "weekly_mb": (0, 100000)}
    labels = {"minimum_active_seconds": "每局最低有效秒数", "daily_matches": "每日累计局数",
              "weekly_matches": "每周累计局数", "daily_first_mb": "每日首局 MB",
              "daily_mb": "每日累计额外 MB", "weekly_mb": "每周累计额外 MB"}
    for key, (low, high) in bounds.items():
        if type(value[key]) is not int or not low <= value[key] <= high:
            raise ValueError(f"{labels[key]} 须为 {low}..{high} 的整数")
    if value["weekly_matches"] < value["daily_matches"]:
        raise ValueError("每周局数不能少于每日局数")
    return value


def solo_policy(policy):
    """Explicit preset; preserve every GB, multiplier and non-MB owner setting."""
    result = copy.deepcopy(policy)
    result["progression"] = {**defaults(), "enabled": True}
    for outcome, mb in (("win", "5"), ("lose", "3"), ("draw", "4")):
        result["pvp"]["base"][outcome]["mb"] = mb
    result["pvp"]["mvp"]["mb"] = "10"
    result["pve"]["base"]["clear"]["mb"] = "2"
    result["pve"]["base"]["fail"]["mb"] = "0.5"
    result["pve"]["total_cap_1x"]["mb"] = "5"
    return result


def periods(completed_unix):
    date = datetime.fromtimestamp(completed_unix, CHINA).date()
    return date.isoformat(), (date - timedelta(days=date.weekday())).isoformat()


def qualifying(job, policy):
    return (policy["enabled"] and job.get("active_seconds", 0) >= policy["minimum_active_seconds"]
            and job.get("contributed") is True)


def public(policy):
    value = validate(policy.get("progression", defaults()))
    return {"config": value, "timezone": "Asia/Shanghai", "week_starts": "Monday",
            "fixed_mb_not_multiplied": True, "requires_native_v2": True,
            "sample_week_mb": 4 * (value["daily_first_mb"] + value["daily_mb"]) + value["weekly_mb"],
            "price_reference": [{"label": "常见单份涂装", "mb": 1100},
                                {"label": "低价普通机体", "mb": 2490},
                                {"label": "部分机体/设计图", "mb": 9900},
                                {"label": "部分高档机体", "mb": 69900},
                                {"label": "部分高档机体上沿", "mb": 79900}]}
