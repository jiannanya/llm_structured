from __future__ import annotations

import json

from llm_structured import parse_and_repair, validate_with_repair


def main() -> None:
    schema = {
        "type": "object",
        "required": ["name", "age"],
        "additionalProperties": False,
        "properties": {
            "name": {"type": "string", "minLength": 1},
            "age": {"type": "integer", "minimum": 0, "maximum": 120},
        },
    }

    # 1) Validate an already-parsed value and get repair suggestions.
    value = {"name": "  Alice  ", "age": "200", "extra": True}
    r1 = validate_with_repair(
        value,
        schema,
        {
            "coerce_types": True,
            "clamp_numbers": True,
            "remove_extra_properties": True,
        },
    )

    print("--- validate_with_repair(value, schema) ---")
    print("valid:", r1["valid"], "fully_repaired:", r1["fully_repaired"])
    print("repaired_value:", json.dumps(r1.get("repaired_value"), ensure_ascii=False))
    print("suggestions:")
    for s in r1.get("suggestions", []):
        print(" -", s.get("path"), s.get("error_kind"), "->", s.get("suggestion"))

    # 2) Parse messy LLM output and auto-repair to the schema.
    llm_output = """
下面是结果（严格 JSON）：

```json
{
  "name": "  Bob  ",
  "age": "42",
  "extra": "please remove",
}
```
"""

    r2 = parse_and_repair(
        llm_output,
        schema,
        {
            "coerce_types": True,
            "clamp_numbers": True,
            "remove_extra_properties": True,
        },
    )

    print("\n--- parse_and_repair(text, schema) ---")
    print("valid:", r2["valid"], "fully_repaired:", r2["fully_repaired"])
    print("repaired_value:")
    print(json.dumps(r2.get("repaired_value"), indent=2, ensure_ascii=False))
    print("unfixable_errors:", len(r2.get("unfixable_errors", [])))


if __name__ == "__main__":
    main()
