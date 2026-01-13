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


# ---- YAML APIs ----

def extract_yaml_candidate(text: str) -> str:
    try:
        return str(_native.extract_yaml_candidate(text)).strip()
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None

def extract_yaml_candidates(text: str) -> list[str]:
    try:
        return list(_native.extract_yaml_candidates(text))
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None


def loads_yamlish(text: str) -> Json:
    try:
        return _native.loads_yamlish(text)
    except ValidationError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None

def loads_yamlish_all(text: str) -> list[Json]:
    try:
        return list(_native.loads_yamlish_all(text))
    except ValidationError:
        raise
    except Exception as exc:  # noqa: BLE001
        raise ValueError(str(exc)) from None


def loads_yamlish_ex(text: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    """Parse YAML-ish text with configurable repairs and return value + metadata."""
    return dict(_native.loads_yamlish_ex(text, repair))

def loads_yamlish_all_ex(text: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    return dict(_native.loads_yamlish_all_ex(text, repair))


def dumps_yaml(value: Json, indent: int = 2) -> str:
    return str(_native.dumps_yaml(value, indent))


def parse_and_validate_yaml(text: str, schema: Schema) -> Json:
    return _native.parse_and_validate_yaml(text, schema)

def parse_and_validate_yaml_all(text: str, schema: Schema) -> list[Json]:
    return list(_native.parse_and_validate_yaml_all(text, schema))


def parse_and_validate_yaml_ex(text: str, schema: Schema, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_yaml_ex(text, schema, repair))

def parse_and_validate_yaml_all_ex(
    text: str, schema: Schema, repair: Mapping[str, Any] | None = None
) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_yaml_all_ex(text, schema, repair))


# ---- TOML APIs ----

def extract_toml_candidate(text: str) -> str:
    """Extract a TOML candidate from LLM text (```toml fence or TOML-like structure)."""
    return str(_native.extract_toml_candidate(text)).strip()


def extract_toml_candidates(text: str) -> list[str]:
    """Extract all TOML candidates from text (multiple ```toml fences)."""
    return list(_native.extract_toml_candidates(text))


def loads_tomlish(text: str) -> Json:
    """Parse TOML-ish text into a Json value (applies best-effort repairs)."""
    return _native.loads_tomlish(text)


def loads_tomlish_all(text: str) -> list[Json]:
    """Parse all TOML documents from text and return them as a list."""
    return list(_native.loads_tomlish_all(text))


def loads_tomlish_ex(text: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    """Parse TOML-ish text with configurable repairs and return value + metadata."""
    return dict(_native.loads_tomlish_ex(text, repair))

def loads_tomlish_all_ex(text: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    return dict(_native.loads_tomlish_all_ex(text, repair))


def dumps_toml(value: Json) -> str:
    """Serialize Json value to TOML string."""
    return str(_native.dumps_toml(value))


def parse_and_validate_toml(text: str, schema: Schema) -> Json:
    return _native.parse_and_validate_toml(text, schema)

def parse_and_validate_toml_all(text: str, schema: Schema) -> list[Json]:
    return list(_native.parse_and_validate_toml_all(text, schema))


def parse_and_validate_toml_ex(text: str, schema: Schema, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_toml_ex(text, schema, repair))

def parse_and_validate_toml_all_ex(
    text: str, schema: Schema, repair: Mapping[str, Any] | None = None
) -> Dict[str, Any]:
    return dict(_native.parse_and_validate_toml_all_ex(text, schema, repair))


# ---- XML / HTML APIs ----

def extract_xml_candidate(text: str) -> str:
    """Extract an XML candidate from LLM text (```xml fence or tag-based)."""
    return str(_native.extract_xml_candidate(text)).strip()


def extract_xml_candidates(text: str) -> list[str]:
    """Extract all XML candidates from text."""
    return list(_native.extract_xml_candidates(text))


def loads_xml(xml_string: str) -> Dict[str, Any]:
    """Parse XML string into an XmlNode tree. Returns {ok, error, root}."""
    return dict(_native.loads_xml(xml_string))


def loads_xml_ex(xml_string: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    """Parse XML with configurable repairs. Returns {ok, error, root, metadata}."""
    return dict(_native.loads_xml_ex(xml_string, repair))


def loads_html(html_string: str) -> Dict[str, Any]:
    """Parse HTML string into an XmlNode tree. Returns {ok, error, root}."""
    return dict(_native.loads_html(html_string))


def loads_html_ex(html_string: str, repair: Mapping[str, Any] | None = None) -> Dict[str, Any]:
    """Parse HTML with configurable repairs. Returns {ok, error, root, metadata}."""
    return dict(_native.loads_html_ex(html_string, repair))


def xml_to_json(xml_string: str) -> Json:
    """Convert XML string to JSON representation."""
    return _native.xml_to_json(xml_string)


def loads_xml_as_json(xml_string: str) -> Json:
    """Parse XML and return as JSON object."""
    return _native.loads_xml_as_json(xml_string)


def loads_html_as_json(html_string: str) -> Json:
    """Parse HTML and return as JSON object."""
    return _native.loads_html_as_json(html_string)


def dumps_xml(node: Dict[str, Any], indent: int = 2) -> str:
    """Serialize XmlNode tree to XML string."""
    return str(_native.dumps_xml(node, indent))


def dumps_html(node: Dict[str, Any], indent: int = 2) -> str:
    """Serialize XmlNode tree to HTML string (handles void elements)."""
    return str(_native.dumps_html(node, indent))


def query_xml(node: Dict[str, Any], selector: str) -> list[Dict[str, Any]]:
    """Query XmlNode tree with simple CSS-like selector (tag, #id, .class)."""
    return list(_native.query_xml(node, selector))


def xml_text_content(node: Dict[str, Any]) -> str:
    """Get concatenated text content of an XmlNode and its children."""
    return str(_native.xml_text_content(node))


def xml_get_attribute(node: Dict[str, Any], attr_name: str) -> str | None:
    """Get attribute value from XmlNode, or None if not found."""
    result = _native.xml_get_attribute(node, attr_name)
    return str(result) if result is not None else None


def validate_xml(node: Dict[str, Any], schema: Schema) -> Dict[str, Any]:
    """Validate XmlNode against schema. Returns {ok, errors}."""
    return dict(_native.validate_xml(node, schema))


def parse_and_validate_xml(xml_string: str, schema: Schema) -> Dict[str, Any]:
    """Parse and validate XML. Returns {ok, error, root, validation_errors}."""
    return dict(_native.parse_and_validate_xml(xml_string, schema))


def parse_and_validate_xml_ex(
    xml_string: str, schema: Schema, repair: Mapping[str, Any] | None = None
) -> Dict[str, Any]:
    """Parse and validate XML with repairs. Returns {ok, error, root, validation_errors, metadata}."""
    return dict(_native.parse_and_validate_xml_ex(xml_string, schema, repair))


# ============== Schema Inference ==============

def infer_schema(value: Json, config: Mapping[str, Any] | None = None) -> Schema:
    """Infer JSON Schema from a single value.
    
    Args:
        value: A JSON-compatible value (dict, list, str, int, float, bool, None)
        config: Optional configuration dict with keys:
            - include_examples: bool (default False) - Include example values
            - max_examples: int (default 3) - Max examples to include
            - include_default: bool (default False) - Include default from first value
            - infer_formats: bool (default True) - Detect string formats (date-time, email, uri, etc.)
            - infer_patterns: bool (default False) - Infer regex patterns
            - infer_numeric_ranges: bool (default False) - Infer minimum/maximum
            - infer_string_lengths: bool (default False) - Infer minLength/maxLength
            - infer_array_lengths: bool (default False) - Infer minItems/maxItems
            - required_by_default: bool (default True) - Make properties required
            - strict_additional_properties: bool (default True) - Set additionalProperties: false
            - prefer_integer: bool (default True) - Use "integer" for whole numbers
            - allow_any_of: bool (default True) - Use anyOf for mixed types
            - detect_enums: bool (default True) - Detect enum from repeated string values
            - max_enum_values: int (default 10) - Max distinct values for enum detection
    
    Returns:
        A JSON Schema dict
    """
    return dict(_native.infer_schema(value, config))


def infer_schema_from_values(values: List[Json], config: Mapping[str, Any] | None = None) -> Schema:
    """Infer JSON Schema from multiple values (merges schemas).
    
    This is useful for inferring a schema from multiple example values,
    where the schema should accept all of them.
    
    Args:
        values: List of JSON-compatible values
        config: Optional configuration (same as infer_schema)
    
    Returns:
        A merged JSON Schema dict that accepts all input values
    """
    return dict(_native.infer_schema_from_values(values, config))


def merge_schemas(schema1: Schema, schema2: Schema, config: Mapping[str, Any] | None = None) -> Schema:
    """Merge two JSON Schemas into one that accepts values valid for either.
    
    Args:
        schema1: First JSON Schema
        schema2: Second JSON Schema  
        config: Optional configuration (same as infer_schema)
    
    Returns:
        A merged JSON Schema
    """
    return dict(_native.merge_schemas(schema1, schema2, config))


# ============== Validation Repair Suggestions ==============

def validate_with_repair(
    value: Json,
    schema: Schema,
    config: Mapping[str, Any] | None = None,
) -> Dict[str, Any]:
    """Validate a value and return repair suggestions / auto-repaired value.

    This does not raise on validation failure. Instead it returns:

    - valid: bool (whether the original value was valid)
    - fully_repaired: bool (whether all validation issues were auto-fixed)
    - repaired_value: the value after applying safe automatic repairs
    - suggestions: list of structured suggestions (each includes path, message, suggestion, etc.)
    - unfixable_errors: remaining validation errors (same shape as validate_all)

    Config keys (all optional):
        - coerce_types: bool
        - use_defaults: bool
        - clamp_numbers: bool
        - truncate_strings: bool
        - truncate_arrays: bool
        - remove_extra_properties: bool
        - fix_enums: bool
        - fix_formats: bool
        - max_suggestions: int
    """
    return dict(_native.validate_with_repair(value, schema, config))


def parse_and_repair(
    text: str,
    schema: Schema,
    config: Mapping[str, Any] | None = None,
    parse_repair: Mapping[str, Any] | None = None,
) -> Dict[str, Any]:
    """Parse JSON-ish text, then validate with repair suggestions.

    - parse_repair: RepairConfig-like dict to control JSON-ish parsing repairs.
    - config: ValidationRepairConfig-like dict to control validation repairs.
    """
    return dict(_native.parse_and_repair(text, schema, config, parse_repair))



__all__ = [
    "Json",
    "Schema",
    "ValidationError",
    "json_pointer_from_path",
    "extract_json_candidate",
    "extract_json_candidates",
    "extract_sql_candidate",
    "extract_yaml_candidate",
    "extract_yaml_candidates",
    "extract_toml_candidate",
    "extract_toml_candidates",
    "extract_xml_candidate",
    "extract_xml_candidates",
    "loads_jsonish",
    "loads_jsonish_all",
    "loads_jsonish_ex",
    "loads_jsonish_all_ex",
    "loads_yamlish",
    "loads_yamlish_all",
    "loads_yamlish_ex",
    "loads_yamlish_all_ex",
    "loads_tomlish",
    "loads_tomlish_all",
    "loads_tomlish_ex",
    "loads_tomlish_all_ex",
    "loads_xml",
    "loads_xml_ex",
    "loads_html",
    "loads_html_ex",
    "xml_to_json",
    "loads_xml_as_json",
    "loads_html_as_json",
    "dumps_json",
    "dumps_yaml",
    "dumps_toml",
    "dumps_xml",
    "dumps_html",
    "query_xml",
    "xml_text_content",
    "xml_get_attribute",
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
    "parse_and_validate_yaml",
    "parse_and_validate_yaml_all",
    "parse_and_validate_yaml_ex",
    "parse_and_validate_yaml_all_ex",
    "parse_and_validate_toml",
    "parse_and_validate_toml_all",
    "parse_and_validate_toml_ex",
    "parse_and_validate_toml_all_ex",
    "validate_xml",
    "parse_and_validate_xml",
    "parse_and_validate_xml_ex",
    "infer_schema",
    "infer_schema_from_values",
    "merge_schemas",
    "validate_with_repair",
    "parse_and_repair",
    "JsonStreamParser",
    "JsonStreamCollector",
    "JsonStreamBatchCollector",
    "JsonStreamValidatedBatchCollector",
    "SqlStreamParser",
]
