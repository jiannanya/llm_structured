from __future__ import annotations

import json
import random
import time

from llm_structured import parse_and_validate


def _make_payload(n: int) -> str:
    items = []
    for i in range(n):
        items.append({"id": i, "score": random.random(), "label": f"item-{i}"})

    obj = {"items": items, "meta": {"count": n}}
    return "```json\n" + json.dumps(obj) + "\n```"


SCHEMA = {
    "type": "object",
    "required": ["items", "meta"],
    "additionalProperties": False,
    "properties": {
        "items": {
            "type": "array",
            "items": {
                "type": "object",
                "required": ["id", "score", "label"],
                "additionalProperties": False,
                "properties": {
                    "id": {"type": "integer", "minimum": 0},
                    "score": {"type": "number", "minimum": 0.0, "maximum": 1.0},
                    "label": {"type": "string", "minLength": 1},
                },
            },
        },
        "meta": {
            "type": "object",
            "required": ["count"],
            "additionalProperties": False,
            "properties": {"count": {"type": "integer", "minimum": 0}},
        },
    },
}


def bench(iterations: int = 2000, item_count: int = 20) -> None:
    payload = _make_payload(item_count)

    # Warm up
    for _ in range(50):
        parse_and_validate(payload, SCHEMA)

    t0 = time.perf_counter()
    for _ in range(iterations):
        parse_and_validate(payload, SCHEMA)
    t1 = time.perf_counter()

    total_s = t1 - t0
    per_call_us = total_s / iterations * 1e6
    print(f"iterations={iterations} item_count={item_count}")
    print(f"total={total_s:.4f}s  per_call={per_call_us:.2f}us")


if __name__ == "__main__":
    bench()
