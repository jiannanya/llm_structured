# llm_structured (C++)

This folder (`cpp/`) contains the C++17 core implementation (standard-library only). The Python and TypeScript packages build on top of the same core logic to keep behavior consistent across languages.

If you only want to use the native library (or embed it into another project), this folder is the source of truth.

## What’s included

- JSON-ish extraction, controlled repair, parsing
- Pragmatic JSON Schema subset validation
- Streaming parsers/collectors (append + poll + finish/location)
- Markdown validation
- Key-value (.env-ish) parsing + validation
- SQL parsing + safety-oriented validation
- Unified error model via `llm_structured::ValidationError` (`kind = schema|type|limit|parse`)

## Build and test (CMake)

From the repository root:

```powershell
cmake -S cpp -B cpp/build
cmake --build cpp/build -j
ctest --test-dir cpp/build -C Debug -V
```

Notes:

- On Windows with Visual Studio generators, pass `-C Debug` (or `-C Release`) to `ctest`.
- If you move the repo folder after configuring, delete `cpp/build/` and re-run `cmake -S ... -B ...`.

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

Duplicate keys inside JSON objects are handled via `repair.duplicate_key_policy`:

- `FirstWins` (default)
- `LastWins`
- `Error` (throws a `ValidationError` with path like `$.a`)

### Parse *all* JSON blocks from one input

If the input contains multiple JSON payloads (multiple fences and/or multiple inline `{...}` / `[...]`), use the `*_all*` APIs.

- Candidates/values are returned in source order.
- Validation errors are rooted at `$[i]` to indicate which block failed.

```cpp
#include <cassert>
#include <string>

#include "llm_structured.hpp"

int main() {
  const std::string fence_open = std::string("`") + "``json\n";
  const std::string fence_close = std::string("`") + "``\n";

  const std::string text =
      "prefix\n" +
      fence_open + "{\"a\": 1}\n" + fence_close +
      "middle {\"b\": 2} tail\n" +
      fence_open + "[1, 2]\n" + fence_close;

  auto candidates = llm_structured::extract_json_candidates(text);
  assert(candidates.size() == 3);

  auto values = llm_structured::loads_jsonish_all(text);
  assert(values.size() == 3);

  llm_structured::Json schema = llm_structured::loads_jsonish(R"JSON(
{"type":"object","required":["a"],"properties":{"a":{"type":"integer"}}}
)JSON");

  try {
    (void)llm_structured::parse_and_validate_all("{\"a\":\"x\"} {\"a\":2}", schema);
  } catch (const llm_structured::ValidationError& e) {
    // e.path is like $[0].a
    (void)e;
  }
}
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

There are also emit-all streaming collectors:

- `JsonStreamCollector` (collect all parsed values)
- `JsonStreamBatchCollector` (emit batches)
- `JsonStreamValidatedBatchCollector` (validate + apply defaults per item)

See the root README for a higher-level overview.

## CLI

### JSON

```powershell
echo '```json\n{"a":1,}\n```' | build/llm_structured_cpp/llm_structured_cli json
```

If you built into `cpp/build/`, the binary path is typically:

- `cpp/build/Debug/llm_structured_cli.exe` (MSVC Debug)
- `cpp/build/Release/llm_structured_cli.exe` (MSVC Release)

With a schema:

```powershell
build/llm_structured_cpp/llm_structured_cli json --schema schema.json < input.txt
```

### SQL

```powershell
echo 'SELECT u.id FROM users u WHERE u.id = ? LIMIT 1' | build/llm_structured_cpp/llm_structured_cli sql --schema sql_schema.json
```
