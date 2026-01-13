from __future__ import annotations

import json

from llm_structured import (
    build_anthropic_tool,
    build_gemini_function_declaration,
    build_openai_function_tool,
    parse_anthropic_tool_uses_from_response,
    parse_gemini_function_calls_from_response,
    parse_openai_tool_calls_from_response,
)


def main() -> None:
    schema = {
        "type": "object",
        "additionalProperties": False,
        "required": ["id"],
        "properties": {"id": {"type": "integer"}},
    }

    print("=== Build tool schemas ===")
    print(json.dumps(build_openai_function_tool("get_user", "Get a user", schema)["tool"], indent=2))
    print(json.dumps(build_anthropic_tool("get_user", "Get a user", schema)["tool"], indent=2))
    print(json.dumps(build_gemini_function_declaration("get_user", "Get a user", schema)["tool"], indent=2))

    schemas_by_name = {"get_user": schema}

    print("\n=== Parse OpenAI response tool_calls ===")
    openai_response = {
        "choices": [
            {
                "message": {
                    "tool_calls": [
                        {
                            "id": "call_1",
                            "type": "function",
                            "function": {"name": "get_user", "arguments": "{'id': '123',}"},
                        }
                    ]
                }
            }
        ]
    }
    openai_calls = parse_openai_tool_calls_from_response(
        openai_response,
        schemas_by_name,
        validation_repair={"coerce_types": True},
        parse_repair={"allowSingleQuotes": True, "dropTrailingCommas": True},
    )
    print(json.dumps(openai_calls, indent=2))

    print("\n=== Parse Anthropic response tool_use ===")
    anthropic_response = {
        "content": [
            {"type": "text", "text": "hi"},
            {"type": "tool_use", "id": "tu_1", "name": "get_user", "input": {"id": "123", "extra": 1}},
        ]
    }
    anthropic_calls = parse_anthropic_tool_uses_from_response(
        anthropic_response,
        schemas_by_name,
        validation_repair={"coerce_types": True, "remove_extra_properties": True},
    )
    print(json.dumps(anthropic_calls, indent=2))

    print("\n=== Parse Gemini response functionCall ===")
    gemini_response = {
        "candidates": [
            {
                "content": {
                    "parts": [
                        {"functionCall": {"name": "get_user", "args": {"id": "123"}}},
                    ]
                }
            }
        ]
    }
    gemini_calls = parse_gemini_function_calls_from_response(
        gemini_response,
        schemas_by_name,
        validation_repair={"coerce_types": True},
    )
    print(json.dumps(gemini_calls, indent=2))


if __name__ == "__main__":
    main()
