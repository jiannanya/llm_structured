import unittest


class TestPythonNativeMore(unittest.TestCase):
    def test_validate_all_json_and_defaults(self) -> None:
        from llm_structured import parse_and_validate_with_defaults, validate_all_json

        v = parse_and_validate_with_defaults(
            '{"name":"Ada"}',
            {
                "type": "object",
                "required": ["name", "age"],
                "properties": {
                    "name": {"type": "string"},
                    "age": {"type": "integer", "default": 18},
                },
            },
        )
        self.assertEqual(v["age"], 18)

        errs = validate_all_json(
            '{"age": -1, "extra": 1}',
            {
                "type": "object",
                "required": ["name"],
                "additionalProperties": False,
                "properties": {
                    "name": {"type": "string", "minLength": 2},
                    "age": {"type": "integer", "minimum": 0},
                },
            },
        )
        self.assertGreaterEqual(len(errs), 2)

    def test_streaming_limits_expose_limit_payload(self) -> None:
        from llm_structured import JsonStreamParser

        p = JsonStreamParser({"type": "object"}, {"maxBufferBytes": 8})
        p.append("0123456789")
        out = p.poll()
        self.assertTrue(out["done"])
        self.assertFalse(out["ok"])
        self.assertIsNotNone(out["error"])
        self.assertEqual(out["error"]["path"], "$.stream.maxBufferBytes")
        self.assertIn("limit", out["error"])
        self.assertEqual(out["error"]["limit"]["kind"], "maxBufferBytes")
        self.assertEqual(out["error"]["limit"]["max"], 8)

    def test_markdown_shape(self) -> None:
        from llm_structured import parse_and_validate_markdown

        parsed = parse_and_validate_markdown(
            "# Title\n\n## Steps\n- one\n- two\n",
            {"requiredHeadings": ["Title", "Steps"], "sections": {"Steps": {"requireBullets": True}}},
        )
        self.assertEqual([h["title"] for h in parsed["headings"]], ["Title", "Steps"])
        self.assertIn("sections", parsed)
        self.assertIn("codeBlocks", parsed)


if __name__ == "__main__":
    unittest.main()
