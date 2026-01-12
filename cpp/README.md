# llm_structured (C++)

This folder (`cpp/`) contains the C++17 core implementation (standard-library only). The Python and TypeScript packages build on top of the same core logic to keep behavior consistent across languages.

## What’s included

- JSON-ish extraction, controlled repair, parsing
- Pragmatic JSON Schema subset validation
- Streaming parsers/collectors (append + poll + finish/location)
- Markdown validation
- Key-value (.env-ish) parsing + validation
- SQL parsing + safety-oriented validation
- Unified error model via `llm_structured::ValidationError` (`kind = schema|type|limit|parse`)

## Build and test (CMake)

From the `test5/test1` folder:

```powershell
cmake -S cpp -B build/llm_structured_cpp
cmake --build build/llm_structured_cpp -j
ctest --test-dir build/llm_structured_cpp -V
```

Artifacts:

- `llm_structured_cli`
- `llm_structured_tests`

## C++ API

Header:

```cpp
#include "llm_structured.hpp"
```

### Parse + validate

```cpp
llm_structured::Json schema = llm_structured::loads_jsonish(R"({
  "type":"object",
  "required":["age"],
  "properties": {"age": {"type":"integer", "minimum": 0}}
})");

auto value = llm_structured::parse_and_validate("```json\n{\"age\": 1}\n```", schema);
```

On failure, `llm_structured::ValidationError` provides:

- `e.what()` / `e.message`
- `e.path` (for example `$.age`)
- `e.kind` (`schema | type | limit | parse`)

### Repairs + metadata (`*_ex`)

```cpp
llm_structured::RepairConfig repair;
repair.drop_trailing_commas = true;
repair.allow_single_quotes = false;

auto r = llm_structured::loads_jsonish_ex("```json\n{\"a\":1,}\n```", repair);
// r.value / r.fixed / r.metadata
```

### Streaming parse (`JsonStreamParser`)

```cpp
llm_structured::JsonStreamParser p(schema, /*max_buffer_bytes=*/1024);
p.append("{\n");
auto loc = p.location();

p.append("\"age\": 1}\n");
p.finish();
auto out = p.poll();
```

## CLI

### JSON

```powershell
echo '```json\n{"a":1,}\n```' | build/llm_structured_cpp/llm_structured_cli json
```

With a schema:

```powershell
build/llm_structured_cpp/llm_structured_cli json --schema schema.json < input.txt
```

### SQL

```powershell
echo 'SELECT u.id FROM users u WHERE u.id = ? LIMIT 1' | build/llm_structured_cpp/llm_structured_cli sql --schema sql_schema.json
```
