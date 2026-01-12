from __future__ import annotations

import json

from llm_structured import ValidationError, parse_and_validate_markdown


def main() -> None:
    # Simulated LLM output: markdown with sections and a python code block.
    llm_output = """
    下面是按要求生成的 Markdown：

    # Release Notes

    ## Summary
    - Add structured output validation
    - Improve error paths

    ## Tasks
    - [ ] write tests
    - [x] update docs

    ```python
    print(\"hello\")
    ```

    以上。
    """

    schema = {
        "requiredHeadings": ["Release Notes", "Summary", "Tasks"],
        "sections": {
            "Summary": {"requireBullets": True, "minBullets": 2},
            "Tasks": {"minLength": 1},
        },
        "requireTaskList": True,
        "requiredCodeFences": ["python"],
        "forbidHtml": True,
        "maxLineLength": 120,
    }

    try:
        parsed = parse_and_validate_markdown(llm_output, schema)
    except ValidationError as e:
        print("Markdown validation failed:", e)
        raise

    print("Markdown validated. Parsed summary:")
    print(
        json.dumps(
            {
                "headings": [h["title"] for h in parsed["headings"]],
                "codeFenceLangs": [b["lang"] for b in parsed["codeBlocks"]],
                "taskLines": parsed["taskLineNumbers"],
                "tables": len(parsed["tables"]),
            },
            indent=2,
            ensure_ascii=False,
        )
    )


if __name__ == "__main__":
    main()
