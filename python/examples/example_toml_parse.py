"""
Example: Parsing TOML from LLM output using llm_structured.

This demonstrates:
1. Basic TOML parsing
2. Parsing with JSON Schema validation
3. Extracting TOML from fenced code blocks
4. Handling arrays of tables
5. Serializing back to TOML
"""

import llm_structured as ls


def main():
    # Example 1: Simple TOML parsing
    print("=== Example 1: Simple TOML parsing ===")
    simple_toml = """
title = "TOML Example"
enabled = true
count = 42

[owner]
name = "Tom Preston-Werner"
dob = 1979-05-27T07:32:00-08:00
"""
    result = ls.loads_tomlish(simple_toml)
    print(f"Parsed: {result}")
    print()

    # Example 2: Parse and validate with JSON Schema
    print("=== Example 2: Parse and validate ===")
    config_toml = """
[server]
host = "localhost"
port = 8080

[database]
name = "myapp"
max_connections = 100
"""
    schema = {
        "type": "object",
        "properties": {
            "server": {
                "type": "object",
                "properties": {
                    "host": {"type": "string"},
                    "port": {"type": "number"}
                },
                "required": ["host", "port"]
            },
            "database": {
                "type": "object",
                "properties": {
                    "name": {"type": "string"},
                    "max_connections": {"type": "number"}
                }
            }
        },
        "required": ["server"]
    }
    validated = ls.parse_and_validate_toml(config_toml, schema)
    print(f"Valid: {validated}")
    print()

    # Example 3: Extract from fenced code block
    print("=== Example 3: Extract from markdown ===")
    llm_output = '''
Here's the configuration you requested:

```toml
[package]
name = "my-app"
version = "1.0.0"

[dependencies]
serde = "1.0"
tokio = { version = "1.0", features = ["full"] }
```

Let me know if you need any changes!
'''
    extracted = ls.extract_toml_candidate(llm_output)
    print(f"Extracted:\n{extracted}")
    parsed = ls.loads_tomlish(llm_output)
    print(f"Parsed: {parsed}")
    print()

    # Example 4: Arrays of tables
    print("=== Example 4: Arrays of tables ===")
    products_toml = """
[[products]]
name = "Hammer"
sku = 738594937

[[products]]
name = "Nail"
sku = 284758393
color = "gray"

[[products]]
name = "Screwdriver"
sku = 847382910
"""
    products = ls.loads_tomlish(products_toml)
    print(f"Products: {products}")
    for i, p in enumerate(products.get("products", [])):
        print(f"  Product {i+1}: {p['name']} (SKU: {p['sku']})")
    print()

    # Example 5: Nested tables and inline tables
    print("=== Example 5: Nested structures ===")
    nested_toml = """
[servers]
  [servers.alpha]
  ip = "10.0.0.1"
  role = "frontend"

  [servers.beta]
  ip = "10.0.0.2"
  role = "backend"

[clients]
data = [["gamma", "delta"], [1, 2]]
"""
    nested = ls.loads_tomlish(nested_toml)
    print(f"Nested: {nested}")
    print()

    # Example 6: Serialize back to TOML
    print("=== Example 6: Serialize to TOML ===")
    data = {
        "title": "My Config",
        "database": {
            "host": "localhost",
            "port": 5432,
            "enabled": True
        },
        "features": ["auth", "cache", "logging"]
    }
    toml_output = ls.dumps_toml(data)
    print(f"Serialized TOML:\n{toml_output}")

    # Example 7: Extended parsing with metadata
    print("\n=== Example 7: Extended parsing with metadata ===")
    result_ex = ls.loads_tomlish_ex(simple_toml)
    print(f"Value: {result_ex['value']}")
    print(f"Metadata: {result_ex['metadata']}")


if __name__ == "__main__":
    main()
