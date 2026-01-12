from __future__ import annotations

import json

from llm_structured import ValidationError, parse_and_validate


def main() -> None:
    # Imagine this comes from an LLM. It often includes explanations or code fences.
    llm_output = """
    下面是结果（严格 JSON）：

    ```json
    {
      "title": "Plan",
      "steps": [
        {"id": 1, "text": "Collect requirements"},
        {"id": 2, "text": "Implement"},
      ]
    }
    ```

    希望对你有帮助。
    """

    schema = {
        "type": "object",
        "required": ["title", "steps"],
        "additionalProperties": False,
        "properties": {
            "title": {"type": "string", "minLength": 1},
            "steps": {
                "type": "array",
                "minItems": 1,
                "items": {
                    "type": "object",
                    "required": ["id", "text"],
                    "additionalProperties": False,
                    "properties": {
                        "id": {"type": "integer", "minimum": 1},
                        "text": {"type": "string", "minLength": 1},
                    },
                },
            },
        },
    }

    try:
        obj = parse_and_validate(llm_output, schema)
    except ValidationError as e:
        print("Validation failed:", e)
        raise

    print("Validated object:")
    print(json.dumps(obj, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
