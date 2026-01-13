import unittest

from llm_structured import (
    ValidationError,
    extract_json_candidate,
    extract_json_candidates,
    loads_jsonish,
    loads_jsonish_all,
    loads_jsonish_ex,
    loads_jsonish_all_ex,
    parse_and_validate,
    parse_and_validate_ex,
    parse_and_validate_json_all,
    parse_and_validate_json_all_ex,
    parse_and_validate_with_defaults_ex,
    parse_and_validate_kv,
    parse_and_validate_markdown,
    parse_and_validate_sql,
    build_openai_function_tool,
    build_anthropic_tool,
    build_gemini_function_declaration,
    parse_openai_tool_call,
    parse_anthropic_tool_use,
    parse_gemini_function_call,
    parse_openai_tool_calls_from_response,
    parse_anthropic_tool_uses_from_response,
    parse_gemini_function_calls_from_response,
    JsonStreamParser,
    JsonStreamValidatedBatchCollector,
)


class TestExtractJsonCandidate(unittest.TestCase):
    def test_extracts_from_code_fence(self) -> None:
        text = """
        blah
        ```json
        {"a": 1}
        ```
        tail
        """
        self.assertEqual(extract_json_candidate(text), '{"a": 1}')

    def test_extracts_first_balanced_object(self) -> None:
        text = "prefix {\"a\": {\"b\": 2}} suffix {\"x\": 1}"
        self.assertEqual(extract_json_candidate(text), '{"a": {"b": 2}}')

    def test_raises_when_missing(self) -> None:
        with self.assertRaises(ValueError):
            extract_json_candidate("no json here")


class TestLoadsJsonish(unittest.TestCase):
    def test_parses_trailing_commas(self) -> None:
        text = """
        ```json
        {"a": 1, "b": [1,2,],}
        ```
        """
        self.assertEqual(loads_jsonish(text), {"a": 1, "b": [1, 2]})

    def test_parses_python_literal_fallback(self) -> None:
        text = "{'a': 1, 'b': True, 'c': None}"
        self.assertEqual(loads_jsonish(text), {"a": 1, "b": True, "c": None})

    def test_rejects_unbalanced(self) -> None:
        with self.assertRaises(ValidationError) as ctx:
            loads_jsonish('{"a": 1')
        self.assertEqual(getattr(ctx.exception, "kind", ""), "parse")

    def test_repair_config_and_metadata(self) -> None:
        r = loads_jsonish_ex("```json\n{'a': True,}\n```", {"allowSingleQuotes": True})
        self.assertEqual(r["value"], {"a": True})
        self.assertTrue(r["metadata"]["extractedFromFence"])
        self.assertTrue(r["metadata"]["replacedPythonLiterals"])
        self.assertTrue(r["metadata"]["droppedTrailingCommas"])

        with self.assertRaises(ValidationError) as ctx:
            loads_jsonish_ex("{'a': 1}", {"allowSingleQuotes": False})
        self.assertEqual(getattr(ctx.exception, "kind", ""), "parse")

    def test_duplicate_key_policy(self) -> None:
        # Default is firstWins (historical behavior).
        r = loads_jsonish_ex('{"a":1,"a":2}', None)
        self.assertEqual(r["value"], {"a": 1})
        self.assertEqual(r["metadata"]["duplicateKeyCount"], 1)
        self.assertEqual(r["metadata"]["duplicateKeyPolicy"], "firstWins")

        r2 = loads_jsonish_ex('{"a":1,"a":2}', {"duplicateKeyPolicy": "lastWins"})
        self.assertEqual(r2["value"], {"a": 2})
        self.assertEqual(r2["metadata"]["duplicateKeyCount"], 1)
        self.assertEqual(r2["metadata"]["duplicateKeyPolicy"], "lastWins")

        with self.assertRaises(ValidationError) as ctx:
            loads_jsonish_ex('{"a":1,"a":2}', {"duplicateKeyPolicy": "error"})
        self.assertEqual(getattr(ctx.exception, "kind", ""), "parse")
        self.assertEqual(getattr(ctx.exception, "path", ""), "$.a")


class TestMultiJsonBlocks(unittest.TestCase):
    def test_extract_json_candidates_all(self) -> None:
        text = (
            "prefix\n"
            "```json\n"
            "{\"a\": 1}\n"
            "```\n"
            "middle {\"b\": 2} tail\n"
            "```json\n"
            "[1, 2]\n"
            "```\n"
        )
        cands = extract_json_candidates(text)
        self.assertEqual(cands, ['{"a": 1}', '{"b": 2}', '[1, 2]'])

    def test_loads_jsonish_all(self) -> None:
        text = (
            "prefix\n"
            "```json\n"
            "{\"a\": 1}\n"
            "```\n"
            "middle {\"b\": 2} tail\n"
            "```json\n"
            "[1, 2]\n"
            "```\n"
        )
        values = loads_jsonish_all(text)
        self.assertEqual(values, [{"a": 1}, {"b": 2}, [1, 2]])

    def test_loads_jsonish_all_ex_has_per_item_metadata(self) -> None:
        text = (
            "prefix\n"
            "```json\n"
            "{\"a\": 1}\n"
            "```\n"
            "middle {\"b\": 2} tail\n"
        )
        r = loads_jsonish_all_ex(text, None)
        self.assertEqual(r["values"], [{"a": 1}, {"b": 2}])
        self.assertEqual(len(r["fixed"]), 2)
        self.assertEqual(len(r["metadata"]), 2)
        self.assertTrue(r["metadata"][0]["extractedFromFence"])
        self.assertFalse(r["metadata"][1]["extractedFromFence"])

    def test_parse_and_validate_json_all_paths_are_indexed(self) -> None:
        text = "{\"a\": \"x\"} {\"a\": 2}"
        schema = {"type": "object", "required": ["a"], "properties": {"a": {"type": "integer"}}}
        with self.assertRaises(ValidationError) as ctx:
            parse_and_validate_json_all(text, schema)
        self.assertTrue(str(ctx.exception).startswith("$[0].a:"))

        r = parse_and_validate_json_all_ex('{"a": 1} {"a": 2}', schema, None)
        self.assertEqual(r["values"], [{"a": 1}, {"a": 2}])
        self.assertEqual(len(r["metadata"]), 2)


class TestValidate(unittest.TestCase):
    def test_parse_and_validate_ok(self) -> None:
        schema = {
            "type": "object",
            "required": ["name", "age"],
            "additionalProperties": False,
            "properties": {
                "name": {"type": "string", "minLength": 1},
                "age": {"type": "integer", "minimum": 0},
            },
        }

        obj = parse_and_validate('{"name": "Ada", "age": 12}', schema)
        self.assertEqual(obj, {"name": "Ada", "age": 12})

    def test_validation_error_path(self) -> None:
        schema = {
            "type": "object",
            "required": ["items"],
            "properties": {
                "items": {
                    "type": "array",
                    "items": {
                        "type": "object",
                        "required": ["id"],
                        "properties": {"id": {"type": "integer"}},
                    },
                }
            },
        }

        with self.assertRaises(ValidationError) as ctx:
            parse_and_validate('{"items": [{"id": "x"}]}', schema)

        self.assertTrue(str(ctx.exception).startswith("$.items[0].id:"))

    def test_additional_properties_rejected(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "properties": {"a": {"type": "integer"}},
        }

        with self.assertRaises(ValidationError):
            parse_and_validate('{"a": 1, "b": 2}', schema)

    def test_schema_new_keywords(self) -> None:
        # format
        parse_and_validate('"a@b.com"', {"type": "string", "format": "email"})
        with self.assertRaises(ValidationError):
            parse_and_validate('"nope"', {"type": "string", "format": "email"})

        # multipleOf
        parse_and_validate("0.3", {"type": "number", "multipleOf": 0.1})
        with self.assertRaises(ValidationError):
            parse_and_validate("0.35", {"type": "number", "multipleOf": 0.1})

        # dependentRequired
        parse_and_validate('{"a":1,"b":2}', {"type": "object", "dependentRequired": {"a": ["b"]}})
        with self.assertRaises(ValidationError):
            parse_and_validate('{"a":1}', {"type": "object", "dependentRequired": {"a": ["b"]}})

        # if/then/else
        schema = {
            "type": "object",
            "if": {"properties": {"type": {"const": "a"}}},
            "then": {"required": ["aVal"]},
            "else": {"required": ["bVal"]},
        }
        with self.assertRaises(ValidationError):
            parse_and_validate('{"type":"a"}', schema)
        with self.assertRaises(ValidationError):
            parse_and_validate('{"type":"b"}', schema)

        # contains + min/max
        schema_contains = {
            "type": "array",
            "contains": {"type": "integer"},
            "minContains": 2,
            "maxContains": 2,
        }
        parse_and_validate("[1,2]", schema_contains)
        with self.assertRaises(ValidationError):
            parse_and_validate("[1]", schema_contains)
        with self.assertRaises(ValidationError):
            parse_and_validate("[1,2,3]", schema_contains)

        # propertyNames
        parse_and_validate('{"ok":1}', {"type": "object", "propertyNames": {"type": "string", "pattern": "^[a-z]+$"}})
        with self.assertRaises(ValidationError):
            parse_and_validate('{"Bad":1}', {"type": "object", "propertyNames": {"type": "string", "pattern": "^[a-z]+$"}})

    def test_ex_variants_return_metadata(self) -> None:
        schema = {"type": "object", "properties": {"age": {"type": "integer", "default": 18}}, "required": ["age"]}
        r1 = parse_and_validate_ex("{'age': 1,}", schema, {"allowSingleQuotes": True})
        self.assertEqual(r1["value"], {"age": 1})
        self.assertIn("metadata", r1)

        r2 = parse_and_validate_with_defaults_ex("{}", schema)
        self.assertEqual(r2["value"], {"age": 18})


class TestStreamingExtras(unittest.TestCase):
    def test_stream_finish_incomplete(self) -> None:
        schema = {"type": "object"}
        p = JsonStreamParser(schema)
        p.append("```json\n{")
        p.finish()
        out = p.poll()
        self.assertTrue(out["done"])
        self.assertFalse(out["ok"])
        self.assertEqual(out["error"]["kind"], "parse")

    def test_validated_batch_applies_defaults(self) -> None:
        schema = {
            "type": "object",
            "required": ["age"],
            "properties": {"age": {"type": "integer", "default": 1}},
        }
        c = JsonStreamValidatedBatchCollector(schema)
        c.append("{}\n")
        out = c.poll()
        self.assertTrue(out["ok"])
        self.assertEqual(out["value"][0]["age"], 1)


class TestMarkdownValidation(unittest.TestCase):
    def test_markdown_required_headings_and_bullets(self) -> None:
        md = """
        # Title

        ## Steps
        - one
        - two

        ```python
        print('ok')
        ```
        """
        schema = {
            "requiredHeadings": ["Title", "Steps"],
            "requiredCodeFences": ["python"],
            "sections": {"Steps": {"requireBullets": True, "minBullets": 2}},
        }

        parsed = parse_and_validate_markdown(md, schema)
        self.assertEqual([h["title"] for h in parsed["headings"]], ["Title", "Steps"])

    def test_markdown_rejects_html_when_forbidden(self) -> None:
        md = "# Title\n\n<div>nope</div>\n"
        with self.assertRaises(ValidationError):
            parse_and_validate_markdown(md, {"forbidHtml": True, "requiredHeadings": ["Title"]})

    def test_markdown_requires_task_list(self) -> None:
        md = "# Tasks\n\n- [ ] one\n- [x] two\n"
        parse_and_validate_markdown(md, {"requiredHeadings": ["Tasks"], "requireTaskList": True})


class TestKeyValueValidation(unittest.TestCase):
    def test_kv_parses_code_fence_and_validates_patterns(self) -> None:
        text = """
        here is config:
        ```env
        API_KEY=abc123
        TIMEOUT=30
        MODE=prod
        ```
        """
        schema = {
            "required": ["API_KEY", "TIMEOUT"],
            "patterns": {"TIMEOUT": r"\d+"},
            "enum": {"MODE": ["dev", "prod"]},
            "allowExtra": True,
        }

        kv = parse_and_validate_kv(text, schema)
        self.assertEqual(kv["API_KEY"], "abc123")
        self.assertEqual(kv["TIMEOUT"], "30")
        self.assertEqual(kv["MODE"], "prod")

    def test_kv_rejects_extra_when_disallowed(self) -> None:
        text = "A=1\nB=2\n"
        with self.assertRaises(ValidationError):
            parse_and_validate_kv(text, {"required": ["A"], "allowExtra": False})


class TestSqlValidation(unittest.TestCase):
    def test_sql_allows_select_and_enforces_where_limit(self) -> None:
        text = """
        ```sql
        SELECT id FROM users WHERE id = 1 LIMIT 10;
        ```
        """
        schema = {
            "allowedStatements": ["select"],
            "requireWhere": True,
            "requireLimit": True,
            "maxLimit": 100,
            "allowedTables": ["users"],
        }
        parsed = parse_and_validate_sql(text, schema)
        self.assertEqual(parsed["statementType"], "select")
        self.assertTrue(parsed["hasWhere"])
        self.assertEqual(parsed["limit"], 10)

    def test_sql_rejects_non_select(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql("DELETE FROM users", {"allowedStatements": ["select"]})

    def test_sql_rejects_multiple_statements(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT 1; SELECT 2;",
                {"allowedStatements": ["select"], "forbidSemicolon": True},
            )

    def test_sql_rejects_union_when_forbidden(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT 1 UNION SELECT 2",
                {"allowedStatements": ["select"], "forbidUnion": True},
            )

    def test_sql_rejects_select_star(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT * FROM users WHERE id = 1 LIMIT 1",
                {"allowedStatements": ["select"], "forbidSelectStar": True},
            )

    def test_sql_requires_where_columns(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM users WHERE id = 1 LIMIT 1",
                {"allowedStatements": ["select"], "requireWhereColumns": ["tenant_id"]},
            )

        parsed = parse_and_validate_sql(
            "SELECT id FROM users WHERE tenant_id = 7 AND id = 1 LIMIT 1",
            {"allowedStatements": ["select"], "requireWhereColumns": ["tenant_id"]},
        )
        self.assertTrue(parsed["hasWhere"])

    def test_sql_forbids_schemas(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT * FROM information_schema.tables WHERE 1=1 LIMIT 1",
                {"allowedStatements": ["select"], "forbidSchemas": ["information_schema"]},
            )

    def test_sql_forbids_cross_join(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT a.id FROM a CROSS JOIN b WHERE a.id = 1 LIMIT 1",
                {"allowedStatements": ["select"], "forbidCrossJoin": True},
            )

    def test_sql_forbids_select_without_limit(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM users WHERE id = 1",
                {"allowedStatements": ["select"], "forbidSelectWithoutLimit": True},
            )

    def test_sql_requires_order_by(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM users WHERE id = 1 LIMIT 1",
                {"allowedStatements": ["select"], "requireOrderBy": True},
            )

        parse_and_validate_sql(
            "SELECT id FROM users WHERE id = 1 ORDER BY id DESC LIMIT 1",
            {"allowedStatements": ["select"], "requireOrderBy": True},
        )

    def test_sql_requires_where_patterns(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM users WHERE id = 1 LIMIT 1",
                {"allowedStatements": ["select"], "requireWherePatterns": [r"\btenant_id\s*="]},
            )

        parse_and_validate_sql(
            "SELECT id FROM users WHERE tenant_id = 7 AND id = 1 LIMIT 1",
            {"allowedStatements": ["select"], "requireWherePatterns": [r"\btenant_id\s*="]},
        )

    def test_sql_forbids_tables(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM secrets WHERE id = 1 LIMIT 1",
                {"allowedStatements": ["select"], "forbidTables": ["secrets"]},
            )

    def test_sql_hardening_allowed_columns_aliases(self) -> None:
        schema = {
            "allowedStatements": ["select"],
            "requireLimit": True,
            "allowedTables": ["users"],
            "allowedColumns": {"users": ["id"]},
            "allowUnqualifiedColumns": False,
            "placeholderStyle": "qmark",
        }
        parse_and_validate_sql("SELECT u.id FROM users u WHERE u.id = ? LIMIT 1", schema)
        with self.assertRaises(ValidationError):
            parse_and_validate_sql("SELECT u.name FROM users u WHERE u.id = ? LIMIT 1", schema)

    def test_sql_hardening_forbid_functions_joins_placeholders_ortrue(self) -> None:
        with self.assertRaises(ValidationError):
            parse_and_validate_sql("SELECT count(id) FROM users LIMIT 1", {"forbidFunctions": ["count"]})

        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT u.id FROM users u JOIN orders o ON o.user_id = u.id LIMIT 1",
                {"maxJoins": 0},
            )


class TestToolCalling(unittest.TestCase):
    def test_build_tool_schemas(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "required": ["id"],
            "properties": {"id": {"type": "integer"}},
        }

        openai = build_openai_function_tool("get_user", "Get a user", schema)
        self.assertEqual(openai["tool"]["type"], "function")
        self.assertEqual(openai["tool"]["function"]["name"], "get_user")
        self.assertIn("parameters", openai["tool"]["function"])

        anthropic = build_anthropic_tool("get_user", "Get a user", schema)
        self.assertEqual(anthropic["tool"]["name"], "get_user")
        self.assertIn("input_schema", anthropic["tool"])

        gemini = build_gemini_function_declaration("get_user", "Get a user", schema)
        self.assertEqual(gemini["tool"]["name"], "get_user")
        self.assertIn("parameters", gemini["tool"])

    def test_parse_openai_tool_call_with_repair(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "required": ["id"],
            "properties": {"id": {"type": "integer"}},
        }

        tool_call = {
            "id": "call_1",
            "type": "function",
            "function": {
                "name": "get_user",
                "arguments": "{'id': '123',}",
            },
        }

        r = parse_openai_tool_call(
            tool_call,
            schema,
            validation_repair={"coerce_types": True},
            parse_repair={"allowSingleQuotes": True, "dropTrailingCommas": True},
        )
        self.assertEqual(r["platform"], "openai")
        self.assertEqual(r["name"], "get_user")
        self.assertTrue(r["ok"])
        self.assertEqual(r["validation"]["repaired_value"], {"id": 123})

    def test_parse_anthropic_tool_use_with_repair(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "required": ["id"],
            "properties": {"id": {"type": "integer"}},
        }

        tool_use = {
            "type": "tool_use",
            "id": "tu_1",
            "name": "get_user",
            "input": {"id": "123", "extra": 1},
        }

        r = parse_anthropic_tool_use(
            tool_use,
            schema,
            validation_repair={"coerce_types": True, "remove_extra_properties": True},
        )
        self.assertEqual(r["platform"], "anthropic")
        self.assertTrue(r["ok"])
        self.assertEqual(r["validation"]["repaired_value"], {"id": 123})

    def test_parse_gemini_function_call_with_repair(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "required": ["id"],
            "properties": {"id": {"type": "integer"}},
        }

        function_call = {"name": "get_user", "args": {"id": "123"}}
        r = parse_gemini_function_call(function_call, schema, validation_repair={"coerce_types": True})
        self.assertEqual(r["platform"], "gemini")
        self.assertTrue(r["ok"])
        self.assertEqual(r["validation"]["repaired_value"], {"id": 123})

    def test_extract_from_responses_and_unknown_schema(self) -> None:
        schema = {
            "type": "object",
            "additionalProperties": False,
            "required": ["id"],
            "properties": {"id": {"type": "integer"}},
        }
        schemas = {"get_user": schema}

        openai_resp = {
            "choices": [
                {
                    "message": {
                        "tool_calls": [
                            {"id": "call_1", "type": "function", "function": {"name": "get_user", "arguments": "{'id':'1'}"}}
                        ]
                    }
                }
            ]
        }
        calls = parse_openai_tool_calls_from_response(
            openai_resp,
            schemas,
            validation_repair={"coerce_types": True},
            parse_repair={"allowSingleQuotes": True},
        )
        self.assertEqual(len(calls), 1)
        self.assertTrue(calls[0]["ok"])

        anthropic_resp = {
            "content": [
                {"type": "text", "text": "hi"},
                {"type": "tool_use", "id": "tu_1", "name": "get_user", "input": {"id": "2"}},
            ]
        }
        uses = parse_anthropic_tool_uses_from_response(anthropic_resp, schemas, validation_repair={"coerce_types": True})
        self.assertEqual(len(uses), 1)
        self.assertTrue(uses[0]["ok"])

        gemini_resp = {
            "candidates": [
                {
                    "content": {
                        "parts": [
                            {"functionCall": {"name": "get_user", "args": {"id": "3"}}},
                        ]
                    }
                }
            ]
        }
        fcs = parse_gemini_function_calls_from_response(gemini_resp, schemas, validation_repair={"coerce_types": True})
        self.assertEqual(len(fcs), 1)
        self.assertTrue(fcs[0]["ok"])

        unknown = parse_openai_tool_calls_from_response(
            {"tool_calls": [{"id": "call_x", "type": "function", "function": {"name": "nope", "arguments": "{}"}}]},
            schemas,
        )
        self.assertEqual(len(unknown), 1)
        self.assertFalse(unknown[0]["ok"])
        self.assertIn("unknown tool schema", unknown[0]["error"])

        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM users WHERE id = ? LIMIT 1",
                {"placeholderStyle": "dollar"},
            )

        with self.assertRaises(ValidationError):
            parse_and_validate_sql(
                "SELECT id FROM users WHERE id = $1 OR 1=1 LIMIT 1",
                {"placeholderStyle": "dollar", "forbidOrTrue": True},
            )


if __name__ == "__main__":
    unittest.main()
