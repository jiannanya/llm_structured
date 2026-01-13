from __future__ import annotations

import json

from llm_structured import (
    extract_xml_candidate,
    loads_xml_ex,
    loads_html_ex,
    dumps_xml,
    dumps_html,
    query_xml,
    xml_text_content,
    xml_get_attribute,
    validate_xml,
    loads_xml_as_json,
)


def main() -> None:
    llm_text = """
下面是 XML：

```xml
<items>
  <item id=a1> Hello </item>
  <item id=b2>World</item>
</items>
```
"""

    xml = extract_xml_candidate(llm_text)
    print("--- Extracted XML ---")
    print(xml)

    parsed = loads_xml_ex(
        xml,
        {
            "fix_unquoted_attributes": True,
            "auto_close_tags": True,
            "decode_entities": True,
        },
    )

    if not parsed["ok"]:
        raise RuntimeError(parsed["error"])

    root = parsed["root"]
    print("\n--- loads_xml_ex metadata ---")
    print(json.dumps(parsed.get("metadata", {}), indent=2, ensure_ascii=False))

    print("\n--- query_xml(root, 'item') ---")
    items = query_xml(root, "item")
    for i, node in enumerate(items):
        print(
            f"item[{i}] id={xml_get_attribute(node, 'id')!r} text={xml_text_content(node)!r}"
        )

    print("\n--- dumps_xml(root) ---")
    print(dumps_xml(root, indent=2))

    print("\n--- loads_xml_as_json(xml) ---")
    as_json = loads_xml_as_json(xml)
    print(json.dumps(as_json, indent=2, ensure_ascii=False))

    # Simple XML schema validation (root-level)
    schema = {
        "element": "items",
        "children": {"minItems": 1, "required": ["item"]},
    }
    v = validate_xml(root, schema)
    print("\n--- validate_xml(root, schema) ---")
    print(v)

    # Failure demo: validate an <item> node directly and require an attribute that does not exist
    bad_item_schema = {
        "element": "item",
        "requiredAttributes": ["id", "missing"],
    }
    v_bad = validate_xml(items[0], bad_item_schema)
    print("\n--- validate_xml(item[0], bad_item_schema) (expected failure) ---")
    print(v_bad)

    # HTML demo (lenient parsing)
    html = """<div class=card><p>Hello<b>world</div>"""
    html_parsed = loads_html_ex(
        html,
        {
            "html_mode": True,
            "fix_unquoted_attributes": True,
            "auto_close_tags": True,
            "lowercase_names": True,
        },
    )
    print("\n--- loads_html_ex(metadata) + dumps_html(root) ---")
    print(json.dumps(html_parsed.get("metadata", {}), indent=2, ensure_ascii=False))
    if html_parsed["ok"] and html_parsed["root"] is not None:
        print(dumps_html(html_parsed["root"], indent=2))


if __name__ == "__main__":
    main()
