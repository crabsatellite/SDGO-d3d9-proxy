def fixture_catalog():
    units = [
        {
            "id": index,
            "name": f"Unit {index}",
            "grade": "C" if index <= 4 else "B",
            "rank": 1 if index <= 4 else 2,
            "subgrade": 2,
            "eligible": True,
            "transform_to": 0,
            "capsule_sprites": [f"sprs\\{index * 2}.spr", f"sprs\\{index * 2 + 1}.spr"],
        }
        for index in range(1, 9)
    ]
    fixed = {
        "id": 101,
        "name": "Native Fixed",
        "enabled": True,
        "owner": "native",
        "currency": "GP",
        "price": 1200,
        "draw_policy": "native_sequence",
        "slots": [
            {
                "unit_id": unit_id,
                "weight": weight,
                "native_draw_group": 1,
                "sprites": units[unit_id - 1]["capsule_sprites"],
            }
            for unit_id, weight in zip(range(1, 7), [20, 80, 250, 250, 250, 150])
        ],
    }
    custom = {
        "id": 1,
        "name": "Native Custom",
        "enabled": True,
        "owner": "native",
        "currency_type": 0,
        "price": 2000,
        "draw_policy": "native_mileage",
        "slot_rules": [
            {
                "slot": index,
                "rank_max": 4,
                "subgrades": [2, 3, 4, 5],
                "extension": index >= 7,
            }
            for index in range(1, 9)
        ],
        "candidates": [
            {
                "unit_id": 7,
                "basic_weight": 100,
                "mileage": 0.5,
                "extra_price": 100,
                "reset_mileage": False,
            }
        ],
    }
    return {
        "schema_version": 1,
        "units": units,
        "fixed_machines": [fixed],
        "custom_machines": [custom],
    }
