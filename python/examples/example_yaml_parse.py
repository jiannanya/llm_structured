"""Example: Parse and validate YAML from LLM output."""

from llm_structured import (
    parse_and_validate_yaml,
    loads_yamlish,
    dumps_yaml,
    extract_yaml_candidates,
    ValidationError,
)

def main():
    # Example 1: Simple YAML parsing
    print("=== Example 1: Simple YAML parsing ===")
    yaml_text = """
```yaml
name: Alice
age: 30
active: true
```
    """
    
    try:
        value = loads_yamlish(yaml_text)
        print(f"Parsed: {value}")
        print(f"Serialized back: {dumps_yaml(value)}")
    except Exception as e:
        print(f"Error: {e}")
    
    # Example 2: Parse and validate
    print("\n=== Example 2: Parse and validate ===")
    yaml_with_schema = """
```yaml
users:
  - name: Alice
    age: 30
  - name: Bob
    age: 25
```
    """
    
    schema = {
        "type": "object",
        "required": ["users"],
        "properties": {
            "users": {
                "type": "array",
                "items": {
                    "type": "object",
                    "required": ["name", "age"],
                    "properties": {
                        "name": {"type": "string"},
                        "age": {"type": "integer", "minimum": 0},
                    },
                },
            },
        },
    }
    
    try:
        result = parse_and_validate_yaml(yaml_with_schema, schema)
        print(f"Valid: {result}")
    except ValidationError as e:
        print(f"Validation error at {e.path}: {e.message}")
    
    # Example 3: Multiple YAML documents
    print("\n=== Example 3: Multiple YAML documents ===")
    multi_yaml = """
Here are two configs:

```yaml
service: api
port: 8080
```

And another one:

```yaml
service: db
port: 5432
```
    """
    
    candidates = extract_yaml_candidates(multi_yaml)
    print(f"Found {len(candidates)} YAML documents:")
    for i, cand in enumerate(candidates):
        print(f"\nDocument {i + 1}:")
        print(cand)
        print(f"Parsed: {loads_yamlish(cand)}")
    
    # Example 4: YAML with repairs
    print("\n=== Example 4: YAML with tab fixes ===")
    yaml_with_tabs = """
```yaml
config:
\tname: test
\tvalue: 123
```
    """
    
    try:
        result = loads_yamlish(yaml_with_tabs)
        print(f"Parsed (tabs fixed): {result}")
    except Exception as e:
        print(f"Error: {e}")

if __name__ == "__main__":
    main()
