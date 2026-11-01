"""Exact, configurable reward DESIGN MODEL, independent of existing reward tables.

This module does not connect to SQL, trust client combat reports or issue money.
The settlement adapter must provide a validated, single-policy match snapshot.
"""
from __future__ import annotations

import copy
from decimal import Decimal, InvalidOperation, localcontext
from fractions import Fraction
import hashlib
import json
from pathlib import Path

CURRENCIES = ("gb", "mb")
DEFAULT_PATH = Path(__file__).with_name("policy.example.json")


def number(value, maximum=1_000_000):
    if isinstance(value, bool) or not isinstance(value, (str, int, float, Decimal)):
        raise ValueError("数值须为非负十进制数")
    try:
        decimal = Decimal(str(value))
    except InvalidOperation:
        raise ValueError("无效的十进制数") from None
    if not decimal.is_finite() or decimal < 0 or decimal > maximum:
        raise ValueError(f"数值须在 0..{maximum} 之间")
    if decimal.as_tuple().exponent < -6:
        raise ValueError("配置最多保留六位小数")
    return Fraction(decimal)


def integer(value, name, maximum):
    if type(value) is not int or not 0 <= value <= maximum:
        raise ValueError(f"{name} 须为 0..{maximum} 的整数")
    return value


def text(value):
    with localcontext() as context:
        context.prec = 40
        result = format(Decimal(value.numerator) / Decimal(value.denominator), "f")
    return result.rstrip("0").rstrip(".") if "." in result else result


def revision(policy):
    return hashlib.sha256(json.dumps(policy, sort_keys=True, separators=(",", ":"),
                                    allow_nan=False).encode()).hexdigest()


def defaults():
    return json.loads(DEFAULT_PATH.read_text(encoding="utf-8"))


def validate(policy):
    try:
        if type(policy["schema_version"]) is not int or policy["schema_version"] != 1:
            raise ValueError("不支持的配置版本")
        number(policy["base_multiplier"], 100)
        if number(policy["extra_human_multiplier"], 2) != Fraction(11, 10):
            raise ValueError("当前真人加成规则固定为每多一人乘 1.1")
        full = integer(policy["full_reward_seconds"], "完整奖励时长", 3600)
        minimum = integer(policy["minimum_active_seconds"], "最低参战时长", 3600)
        if not 0 < minimum <= full:
            raise ValueError("参战时长须满足 0 < 最低时长 <= 完整奖励时长")
        for mode, results, components in (
            ("pvp", ("win", "lose", "draw"), ("kill", "assist", "damage_per_1000", "objective", "contribution_cap", "mvp")),
            ("pve", ("clear", "fail"), ("minion", "boss_credit", "damage_per_1000", "objective", "contribution_cap", "total_cap_1x")),
        ):
            table = policy[mode]
            for item in [*(table["base"][r] for r in results), *(table[c] for c in components)]:
                for currency in CURRENCIES:
                    number(item[currency])
        for difficulty in ("normal", "hard", "expert"):
            if not 0 < number(policy["pve"]["difficulty"][difficulty], 3):
                raise ValueError("难度倍率须大于零")
    except (KeyError, TypeError):
        raise ValueError("奖励配置字段缺失或类型不正确") from None
    revision(policy)
    return policy


def preview(policy, context, stats):
    """Compute one participant from a server-supplied context; AI opponents get no discount.

    Only the owner preview accepts arbitrary input. A live caller must supply
    authoritative stats, unique qualifying human count and native MVP status.
    """
    validate(policy)
    mode = context.get("mode")
    if mode not in ("pvp", "pve"):
        raise ValueError("模式须为 pvp 或 pve")
    humans = integer(context.get("human_count"), "有效真人数", 12)
    if not humans:
        raise ValueError("有效真人数至少为一")
    event = number(context.get("event_multiplier", "1"), 2)
    if event not in (1, 2):
        raise ValueError("双倍时段组件只应提供一倍或两倍，不再重复乘钱")
    active = integer(stats.get("active_seconds", 0), "有效参战秒数", 86400)
    duration = integer(context.get("duration_seconds", active), "本局秒数", 86400)
    if active > duration:
        raise ValueError("有效参战时长不能超过本局时长")
    kind = stats.get("kind", "human")
    if kind not in ("human", "ai", "observer"):
        raise ValueError("参与者类型无效")
    if type(stats.get("is_mvp", False)) is not bool:
        raise ValueError("MVP 标记须为布尔值")
    counts = {key: integer(stats.get(key, 0), key, maximum) for key, maximum in
              (("kills", 255), ("assists", 255), ("minion_kills", 10000),
               ("damage", 100_000_000), ("objectives", 1000))}
    bosses = integer(context.get("bosses_defeated", 0), "本局击破 Boss 数", 100)
    boss_credit = number(stats.get("boss_credit", "0"), 100)
    if boss_credit > bosses:
        raise ValueError("个人 Boss 贡献折算不能超过本局实际击破数")
    relevant = ("kills", "assists", "damage", "objectives") if mode == "pvp" else ("minion_kills", "damage", "objectives")
    meaningful = any(counts[key] for key in relevant) or (mode == "pve" and boss_credit > 0)
    reason = ("ai_or_observer" if kind != "human" else
              "too_short" if active < policy["minimum_active_seconds"] else "eligible")
    table = policy[mode]
    outcome = context.get("outcome")
    if outcome not in table["base"]:
        raise ValueError("本局结果与模式不匹配")
    difficulty = Fraction(1)
    if mode == "pve":
        selected = context.get("difficulty", "normal")
        if selected not in table["difficulty"]:
            raise ValueError("任务难度无效")
        difficulty = number(table["difficulty"][selected], 3)
    multiplier = {
        "base": number(policy["base_multiplier"], 100),
        "double_time": event,
        "humans": number(policy["extra_human_multiplier"], 2) ** (humans - 1),
        "duration": min(Fraction(1), Fraction(active, policy["full_reward_seconds"])),
    }
    factor = multiplier["base"] * event * multiplier["humans"] * multiplier["duration"]
    components = []
    def component(name, units, rate):
        result = {c: units * number(rate[c]) for c in CURRENCIES}
        components.append({"name": name, **{c: text(result[c]) for c in CURRENCIES}})
        return result
    base = component("base", Fraction(1), table["base"][outcome])
    contribution = {c: Fraction(0) for c in CURRENCIES}
    entries = (("kills", counts["kills"], "kill"), ("assists", counts["assists"], "assist")) if mode == "pvp" else (
        ("minions", counts["minion_kills"], "minion"), ("boss_credit", boss_credit, "boss_credit"))
    for name, units, rate in (*entries, ("damage", Fraction(counts["damage"], 1000), "damage_per_1000"),
                              ("objectives", counts["objectives"], "objective")):
        value = component(name, units, table[rate])
        for c in CURRENCIES:
            contribution[c] += value[c]
    capped = {c: min(contribution[c], number(table["contribution_cap"][c])) for c in CURRENCIES}
    components.append({"name": "contribution_cap_adjustment", **{c: text(capped[c] - contribution[c]) for c in CURRENCIES}})
    mvp = component("mvp", Fraction(int(mode == "pvp" and meaningful and stats.get("is_mvp", False))),
                    table["mvp"] if mode == "pvp" else {"gb": "0", "mb": "0"})
    raw = {c: (base[c] + capped[c] + mvp[c]) * difficulty for c in CURRENCIES}
    if mode == "pve":
        raw = {c: min(raw[c], number(table["total_cap_1x"][c])) for c in CURRENCIES}
    amounts = {c: (raw[c] * factor).numerator // (raw[c] * factor).denominator if reason == "eligible" else 0 for c in CURRENCIES}
    if any(amount > 2147483647 for amount in amounts.values()):
        raise ValueError("奖励超出原生钱包整数范围，拒绝计算")
    return {"policy_revision": revision(policy), "eligible": reason == "eligible", "reason": reason,
            "amount": amounts, "base_1x": {c: text(raw[c]) for c in CURRENCIES},
            "components": components, "difficulty_multiplier": text(difficulty),
            "multipliers": {**{key: text(value) for key, value in multiplier.items()}, "effective": text(factor)},
            "delivery_connected": False, "rounding": "floor_once_after_all_multipliers"}


def with_multiplier(policy, value):
    result = copy.deepcopy(policy)
    result["base_multiplier"] = text(number(value, 100))
    return validate(result)


def preview_match(policy, context, participants):
    """Batch design contract: one snapshot, unique accounts, derived human count.

    The future native collector supplies the participants. This remains a pure
    calculation and is deliberately not an authenticated payout endpoint.
    """
    policy, context, participants = copy.deepcopy((policy, context, participants))
    validate(policy)
    if not isinstance(participants, list) or not 1 <= len(participants) <= 12:
        raise ValueError("本局参与者须为 1..12 条唯一记录")
    identities, accounts = set(), set()
    humans = 0
    total_credit = Fraction(0)
    for participant in participants:
        identity = participant["participant_id"]
        if not isinstance(identity, str) or not identity or identity in identities:
            raise ValueError("本局参与者身份重复或无效")
        identities.add(identity)
        stats = participant["stats"]
        kind = stats.get("kind", "human")
        account = participant.get("account_id")
        if kind == "human":
            if type(account) is not int or not 0 < account <= 2147483647 or account in accounts:
                raise ValueError("真人钱包账号重复或无效；重连须先合并为同一参与者")
            accounts.add(account)
            if integer(stats.get("active_seconds", 0), "有效参战秒数", 86400) >= policy["minimum_active_seconds"]:
                humans += 1
        elif account is not None:
            raise ValueError("AI 和观战记录不能携带奖励钱包账号")
        total_credit += number(stats.get("boss_credit", "0"), 100)
    if context.get("mode") == "pve" and total_credit > integer(context.get("bosses_defeated", 0), "本局击破 Boss 数", 100):
        raise ValueError("全场 Boss 贡献合计超过实际击破数")
    context["human_count"] = max(1, humans)  # neutral factor; no eligible humans means every payout is zero
    results = [{"participant_id": p["participant_id"], "account_id": p.get("account_id"),
                **preview(policy, context, p["stats"])} for p in participants]
    return {"policy_revision": revision(policy), "human_count": humans, "participants": results,
            "delivery_connected": False}
