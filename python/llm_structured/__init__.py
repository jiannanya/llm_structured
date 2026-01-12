from __future__ import annotations

from typing import Any, Dict, List, Mapping, Union

Json = Union[None, bool, int, float, str, List["Json"], Dict[str, "Json"]]
Schema = Mapping[str, Any]

try:
    from . import _native as _native  # type: ignore
except Exception as exc:  # noqa: BLE001
    raise ImportError(
        "Native extension llm_structured._native is not available. "
        "Build it from test5/test1/python via: python -m pip install -e ."
    ) from exc


ValidationError = _native.ValidationError  # type: ignore[misc]

json_pointer_from_path = _native.json_pointer_from_path
extract_sql_candidate = _native.extract_sql_candidate

JsonStreamParser = _native.JsonStreamParser
JsonStreamCollector = _native.JsonStreamCollector
JsonStreamBatchCollector = _native.JsonStreamBatchCollector
JsonStreamValidatedBatchCollector = _native.JsonStreamValidatedBatchCollector
SqlStreamParser = _native.SqlStreamParser


def extract_json_candidate(text: str) -> str:
    try:
        return str(_native.extract_json_candidate(text)).strip()
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None

def extract_json_candidates(text: str) -> list[str]:
    try:
        return list(_native.extract_json_candidates(text))
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None


def loads_jsonish(text: str) -> Json:
    try:
        return _native.loads_jsonish(text)
    except ValidationError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None

def loads_jsonish_all(text: str) -> list[Json]:
    try:
        return list(_native.loads_jsonish_all(text))
    except ValidationError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None


def loads_jsonish_ex(text: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    """Parse JSON-ish text with configurable repairs and return value + metadata."""
    return dict(_native.loads_jsonish_ex(text, repair))

def loads_jsonish_all_ex(text: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    return dict(_native.loads_jsonish_all_ex(text, repair))


def dumps_json(value: Json) -> str:
    return str(_native.dumps_json(value))


def validate(value: Json, schema: Schema, path: str = "$") -> None:
    _native.validate_json_value(value, schema, path)


def validate_all(value: Json, schema: Schema, path: str = "$") -> List[Dict[str, Any]]:
    return list(_native.validate_all_json_value(value, schema, path))


def parse_and_validate(text: str, schema: Schema) -> Json:
    return _native.parse_and_validate_json(text, schema)

def parse_and_validate_json_all(text: str, schema: Schema) -> list[Json]:
    return list(_native.parse_and_validate_json_all(text, schema))


def parse_and_validate_ex(text: str, schema: Schema, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_json_ex(text, schema, repair))

def parse_and_validate_json_all_ex(
    text: str, schema: Schema, repair: Mapping[str, Any] | None = None
) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_json_all_ex(text, schema, repair))


def parse_and_validate_with_defaults(text: str, schema: Schema) -> Json:
    return _native.parse_and_validate_json_with_defaults(text, schema)


def parse_and_validate_with_defaults_ex(
    text: str, schema: Schema, repair: Mapping[str, Any] | None = None
) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_json_with_defaults_ex(text, schema, repair))


def validate_all_json(text: str, schema: Schema) -> List[Dict[str, Any]]:
    return list(_native.validate_all_json(text, schema))


def parse_and_validate_markdown(text: str, schema: Schema) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_markdown(text, schema))


def parse_and_validate_kv(text: str, schema: Schema) -> Dict[str, str]:
    return dict(_native.parse_and_validate_kv(text, schema))


def parse_and_validate_sql(text: str, schema: Schema) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_sql(text, schema))


__all__ = [
    "Json",
    "Schema",
    "ValidationError",
    "json_pointer_from_path",
    "extract_json_candidate",
    "extract_json_candidates",
    "extract_sql_candidate",
    "loads_jsonish",
    "loads_jsonish_all",
    "loads_jsonish_ex",
    "loads_jsonish_all_ex",
    "dumps_json",
    "validate",
    "validate_all",
    "validate_all_json",
    "parse_and_validate",
    "parse_and_validate_json_all",
    "parse_and_validate_ex",
    "parse_and_validate_json_all_ex",
    "parse_and_validate_with_defaults",
    "parse_and_validate_with_defaults_ex",
    "parse_and_validate_markdown",
    "parse_and_validate_kv",
    "parse_and_validate_sql",
    "JsonStreamParser",
    "JsonStreamCollector",
    "JsonStreamBatchCollector",
    "JsonStreamValidatedBatchCollector",
    "SqlStreamParser",
]
