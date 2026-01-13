"""
Schema Inference Example

This example demonstrates how to automatically infer JSON Schema from example values.
"""

import json
from llm_structured import infer_schema, infer_schema_from_values, merge_schemas


def main():
    print("=" * 60)
    print("Schema Inference Examples")
    print("=" * 60)

    # Example 1: Basic schema inference from a single value
    print("\n1. Basic schema inference:")
    print("-" * 40)
    
    user = {
        "name": "Alice",
        "age": 30,
        "email": "alice@example.com",
        "active": True
    }
    
    schema = infer_schema(user)
    print(f"Input: {json.dumps(user)}")
    print(f"Inferred schema:\n{json.dumps(schema, indent=2)}")

    # Example 2: Format detection
    print("\n2. Format detection (email, date-time, uri):")
    print("-" * 40)
    
    data = {
        "timestamp": "2024-01-15T10:30:00Z",
        "email": "test@example.com",
        "website": "https://example.com",
        "id": "550e8400-e29b-41d4-a716-446655440000"
    }
    
    schema = infer_schema(data, {"infer_formats": True})
    print(f"Input: {json.dumps(data)}")
    print(f"Inferred schema:\n{json.dumps(schema, indent=2)}")

    # Example 3: Schema inference from multiple values
    print("\n3. Schema inference from multiple values:")
    print("-" * 40)
    
    users = [
        {"name": "Alice", "age": 30, "email": "alice@example.com"},
        {"name": "Bob", "age": 25, "city": "New York"},
        {"name": "Carol", "age": 35, "email": "carol@example.com", "city": "Boston"}
    ]
    
    schema = infer_schema_from_values(users)
    print(f"Input: {len(users)} user objects")
    print(f"Merged schema:\n{json.dumps(schema, indent=2)}")
    print("\nNote: 'required' only contains fields present in ALL examples")

    # Example 4: Enum detection
    print("\n4. Enum detection from repeated values:")
    print("-" * 40)
    
    statuses = [
        {"status": "active"},
        {"status": "inactive"},
        {"status": "pending"},
        {"status": "active"},
        {"status": "inactive"}
    ]
    
    schema = infer_schema_from_values(statuses, {
        "detect_enums": True,
        "max_enum_values": 10
    })
    print(f"Input: 5 status objects with 3 unique values")
    print(f"Inferred schema:\n{json.dumps(schema, indent=2)}")

    # Example 5: Numeric range inference
    print("\n5. Numeric range inference:")
    print("-" * 40)
    
    products = [
        {"price": 10.99, "quantity": 5},
        {"price": 25.50, "quantity": 2},
        {"price": 7.25, "quantity": 10}
    ]
    
    schema = infer_schema_from_values(products, {"infer_numeric_ranges": True})
    print(f"Input: 3 product objects")
    print(f"Inferred schema:\n{json.dumps(schema, indent=2)}")

    # Example 6: Schema merging
    print("\n6. Schema merging:")
    print("-" * 40)
    
    schema1 = {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "age": {"type": "integer"}
        },
        "required": ["name", "age"]
    }
    
    schema2 = {
        "type": "object",
        "properties": {
            "name": {"type": "string"},
            "email": {"type": "string", "format": "email"}
        },
        "required": ["name", "email"]
    }
    
    merged = merge_schemas(schema1, schema2)
    print(f"Schema 1: {json.dumps(schema1)}")
    print(f"Schema 2: {json.dumps(schema2)}")
    print(f"Merged schema:\n{json.dumps(merged, indent=2)}")

    # Example 7: Array schema inference
    print("\n7. Array schema inference:")
    print("-" * 40)
    
    data = {
        "tags": ["python", "javascript", "typescript"],
        "scores": [85, 92, 78]
    }
    
    schema = infer_schema(data)
    print(f"Input: {json.dumps(data)}")
    print(f"Inferred schema:\n{json.dumps(schema, indent=2)}")

    # Example 8: Nested object inference
    print("\n8. Nested object inference:")
    print("-" * 40)
    
    data = {
        "user": {
            "name": "Alice",
            "address": {
                "city": "Boston",
                "zip": "02101"
            }
        },
        "created_at": "2024-01-15T10:30:00Z"
    }
    
    schema = infer_schema(data)
    print(f"Input: {json.dumps(data)}")
    print(f"Inferred schema:\n{json.dumps(schema, indent=2)}")

    print("\n" + "=" * 60)
    print("All examples completed successfully!")
    print("=" * 60)


if __name__ == "__main__":
    main()
