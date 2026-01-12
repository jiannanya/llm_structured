from __future__ import annotations

import json

from llm_structured import ValidationError, parse_and_validate_kv


def main() -> None:
    llm_output = """
    Here is your .env:

    ```env
    API_KEY=abc123
    TIMEOUT=30
    MODE=prod
    ```
    """

    schema = {
        "required": ["API_KEY", "TIMEOUT"],
        "patterns": {"TIMEOUT": r"\\d+"},
        "enum": {"MODE": ["dev", "prod"]},
        "allowExtra": False,
    }

    try:
        kv = parse_and_validate_kv(llm_output, schema)
    except ValidationError as e:
        print("KV validation failed:", e)
        raise

    print("KV validated:")
    print(json.dumps(kv, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
