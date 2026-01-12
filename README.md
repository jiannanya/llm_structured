# llm_structured

**Extract, repair, parse, and validate structured outputs from LLM messy text or output — consistently across C++, Python, and TypeScript.**

>Parsing you can trust—turns ambiguity into clarity.

>Parsing that respects your time: fewer surprises, more signal.

A powerful cross-language toolkit for extracting JSON-like payloads from mixed text, applying controlled repairs (JSON-ish normalization), parsing, and validating with a pragmatic schema subset.

It also includes validators for Markdown, key-value (.env-ish) text, and SQL, plus incremental (streaming) parsers.

The same core behavior is shared across implementations (C++ core + bindings), so you can reuse schemas and expect consistent error fields.

## Features

### JSON-ish parsing

Typical inputs include:

- A JSON object/array embedded inside other text
- A fenced code block (for example, one starting with `\`\`\`json`)
- Trailing commas
- Smart quotes
- Occasional Python literals (`True`, `False`, `None`)

Repairs are explicit and configurable via `RepairConfig`, and the extended APIs (`*_ex`) return:

- `value`: parsed value
- `fixed`: repaired text
- `metadata`: which repairs actually happened

Supported repairs (opt-in via `RepairConfig`) include:

- `fixSmartQuotes`: normalize curly quotes to ASCII quotes
- `stripJsonComments`: remove `// ...` and `/* ... */` comments
- `replacePythonLiterals`: translate Python-y literals (`True`/`False`/`None`) to JSON (`true`/`false`/`null`)
- `convertKvObjectToJson`: accept loose `key=value` blobs and convert to JSON objects
- `quoteUnquotedKeys`: allow `{a: 1}`-style objects by quoting keys
- `dropTrailingCommas`: remove trailing commas in objects/arrays
- `allowSingleQuotes`: allow single-quoted strings/keys when parsing

Duplicate keys inside objects are handled via `duplicateKeyPolicy`:

- `firstWins` (default): keep the first occurrence (backwards compatible)
- `lastWins`: overwrite with the last occurrence
- `error`: reject and raise a parse error with a specific key path (for example `$.a`)

### Multi-JSON-block parsing (parse *all* blocks)

If your input contains multiple JSON blobs (for example, several \`\`\`json fences plus inline `{...}` / `[...]`), you can extract/parse **all** blocks instead of only the first one.

Semantics:

- Candidates are returned in **source order** (left-to-right in the original text).
- Validation errors are rooted at `$[i]` to indicate which block failed (for example `$[0].a`).
- `*_all_ex` returns per-block `fixed[]` and `metadata[]` aligned with `values[]`.

APIs:

- C++: `extract_json_candidates`, `loads_jsonish_all`, `loads_jsonish_all_ex`, `parse_and_validate_all`, `parse_and_validate_all_ex`
- Python: `extract_json_candidates`, `loads_jsonish_all`, `loads_jsonish_all_ex`, `parse_and_validate_json_all`, `parse_and_validate_json_all_ex`
- TypeScript: `extractJsonCandidates`, `loadsJsonishAll`, `loadsJsonishAllEx`, `parseAndValidateJsonAll`, `parseAndValidateJsonAllEx`

### Validation and errors

Validation is performed against a pragmatic schema subset (see Schema support below).

Errors carry consistent fields across languages:

- `kind`: `schema` | `type` | `limit` | `parse`
- `path`: JSONPath-ish location (for example `$.steps[0].id`)
- `jsonPointer` (Python/TypeScript): JSON Pointer (for example `/steps/0/id`)

### Validators (SQL / Markdown / key-value)

In addition to JSON schema validation:

- SQL: parse and validate against an allowlist-style policy (statements/tables/keywords/placeholders/limits)
- Markdown: validate document structure (headings/sections/bullets/code fences/tables/task lists)
- Key-value: validate `.env`-ish text (required keys, allowed extras, regex patterns, enum lists)

### Streaming parsers (incremental)

For LLM streaming output, the library provides incremental parsers/collectors:

- JSON: emit-first parser and emit-all collectors, with optional limits (`maxBufferBytes`, `maxItems`)
- SQL: incremental parsing/validation for streaming SQL output

Core methods are consistent across languages: `append(...)`, `poll()`, plus `finish()`/`close()` and `location()`.

### Cross-language consistency + CLI

- C++17 core library shared by the Python (pybind11) and TypeScript (Node-API) bindings
- Consistent error shape (`kind`, `path`, `jsonPointer`) and repair metadata across languages
- C++ CLI for quick validation from the terminal (see CLI section below)

## Quickstart

The commands below assume you run them from the repository root.

### Python

Build/install the editable package:

```powershell
python -m pip install -r python/requirements-build.txt
python -m pip install -e python
```

Run examples:

```powershell
python python/examples/example_structured_output.py
python python/examples/example_sql_validate.py
```

Run tests:

```powershell
python -m unittest discover -s python/test -p "test_*.py" -v
```

### TypeScript / Node

```powershell
Set-Location typescript
npm install
npm test
```

If PowerShell blocks `npm` because of script execution policy, use one of these options:

- Run `npm.cmd` explicitly (PowerShell can invoke it without `npm.ps1`).
- Or run the commands from `cmd.exe` (for example: `cmd /c npm install`).

### C++ (JSON)

```powershell
cmake -S cpp -B cpp/build
cmake --build cpp/build -j
ctest --test-dir cpp/build -C Debug -V
```

Artifacts:

- `llm_structured_cli`
- `llm_structured_tests`

## Example 1: parse + validate an embedded JSON-ish payload (C++ / Python / TypeScript)

This example uses the same schema and the same input across all three languages:

- The input is fenced and contains a trailing comma.
- The schema expects an object with `title` and an array of `steps`.

Input text:

````text
Here is the payload:

```json
{"title":"Plan","steps":[{"id":1,"text":"Write docs"}],}
```
````

### C++

```cpp
#include "llm_structured.hpp"

int main() {
  llm_structured::Json schema = llm_structured::loads_jsonish(R"JSON(
{
  "type": "object",
  "required": ["title", "steps"],
  "additionalProperties": false,
  "properties": {
    "title": {"type": "string", "minLength": 1},
    "steps": {
      "type": "array",
      "minItems": 1,
      "items": {
        "type": "object",
        "required": ["id", "text"],
        "additionalProperties": false,
        "properties": {
          "id": {"type": "integer", "minimum": 1},
          "text": {"type": "string", "minLength": 1}
        }
      }
    }
  }
}
)JSON");

  try {
    const std::string fence_open = std::string("`") + "``json\n";
    const std::string fence_close = std::string("`") + "``\n";
    auto obj = llm_structured::parse_and_validate(
      "Here is the payload:\n\n" +
        fence_open +
        "{\"title\":\"Plan\",\"steps\":[{\"id\":1,\"text\":\"Write docs\"}],}\n" +
        fence_close,
                                                 schema);
    (void)obj;
  } catch (const llm_structured::ValidationError& e) {
    // e.kind: schema | type | limit | parse
    // e.path: $.steps[0].id
    throw;
  }
}
```

### Python (JSON)

```python
from llm_structured import ValidationError, parse_and_validate

schema = {
  "type": "object",
  "required": ["title", "steps"],
  "additionalProperties": False,
  "properties": {
    "title": {"type": "string", "minLength": 1},
    "steps": {
      "type": "array",
      "minItems": 1,
      "items": {
        "type": "object",
        "required": ["id", "text"],
        "additionalProperties": False,
        "properties": {
          "id": {"type": "integer", "minimum": 1},
          "text": {"type": "string", "minLength": 1},
        },
      },
    },
  },
}

fence_open = "`" + "``json\n"
fence_close = "`" + "``\n"
text = "Here is the payload:\n\n" + fence_open + "{\"title\":\"Plan\",\"steps\":[{\"id\":1,\"text\":\"Write docs\"}],}\n" + fence_close

try:
  obj = parse_and_validate(text, schema)
  print(obj)
except ValidationError as e:
  # e.kind / e.path / e.jsonPointer
  raise
```

### TypeScript (JSON)

```ts
import { parseAndValidateJson, type ValidationError } from "./src/index";

const schema = {
  type: "object",
  required: ["title", "steps"],
  additionalProperties: false,
  properties: {
    title: { type: "string", minLength: 1 },
    steps: {
      type: "array",
      minItems: 1,
      items: {
        type: "object",
        required: ["id", "text"],
        additionalProperties: false,
        properties: {
          id: { type: "integer", minimum: 1 },
          text: { type: "string", minLength: 1 },
        },
      },
    },
  },
} as const;

const fenceOpen = "`" + "``json\n";
const fenceClose = "`" + "``\n";
const text = "Here is the payload:\n\n" + fenceOpen + "{\"title\":\"Plan\",\"steps\":[{\"id\":1,\"text\":\"Write docs\"}],}\n" + fenceClose;

try {
  const obj = parseAndValidateJson(text, schema);
  console.log(obj);
} catch (e) {
  const err = e as ValidationError;
  throw err;
}
```

## Example 1b: parse *all* JSON blocks in one input (C++ / Python / TypeScript)

Input text containing multiple JSON payloads:

````text
prefix
```json
{"a": 1}
```
middle {"b": 2} tail
```json
[1, 2]
```
````

### C++ (all blocks)

```cpp
#include <cassert>
#include <string>

#include "llm_structured.hpp"

int main() {
  // Build a text that contains multiple JSON payloads.
  const std::string fence_open = std::string("`") + "``json\n";
  const std::string fence_close = std::string("`") + "``\n";

  const std::string text =
      "prefix\n" +
      fence_open +
      "{\"a\": 1}\n" +
      fence_close +
      "middle {\"b\": 2} tail\n" +
      fence_open +
      "[1, 2]\n" +
      fence_close;

  // 1) Extract all candidates (in source order).
  auto cands = llm_structured::extract_json_candidates(text);
  assert(cands.size() == 3);

  // 2) Parse all values.
  auto values = llm_structured::loads_jsonish_all(text);
  assert(values.size() == 3);

  // 3) Validate all blocks. Errors are rooted at $[i].
  llm_structured::Json schema = llm_structured::loads_jsonish(R"JSON(
{"type":"object","required":["a"],"properties":{"a":{"type":"integer"}}}
)JSON");

  try {
    (void)llm_structured::parse_and_validate_all("{\"a\":\"x\"} {\"a\":2}", schema);
    assert(false && "expected ValidationError");
  } catch (const llm_structured::ValidationError& e) {
    // e.path is like $[0].a
    (void)e;
  }

  return 0;
}
```

### Python (all blocks)

```python
from llm_structured import extract_json_candidates, loads_jsonish_all, parse_and_validate_json_all

text = (
  "prefix\n"
  "`" "``json\n{\"a\": 1}\n`" "``\n"
  "middle {\"b\": 2} tail\n"
  "`" "``json\n[1, 2]\n`" "``\n"
)

print(extract_json_candidates(text))
print(loads_jsonish_all(text))

schema = {"type": "object", "required": ["a"], "properties": {"a": {"type": "integer"}}}
# Raises ValidationError with path rooted at $[i] if any block fails.
parse_and_validate_json_all('{"a":"x"} {"a":2}', schema)
```

### TypeScript (all blocks)

```ts
import { extractJsonCandidates, loadsJsonishAll, parseAndValidateJsonAll, type ValidationError } from "./src/index";

const text =
  "prefix\n" +
  "`" + "``json\n{\"a\": 1}\n" + "`" + "``\n" +
  "middle {\"b\": 2} tail\n" +
  "`" + "``json\n[1, 2]\n" + "`" + "``\n";

console.log(extractJsonCandidates(text));
console.log(loadsJsonishAll(text));

try {
  parseAndValidateJsonAll('{"a":"x"} {"a":2}', {
    type: "object",
    required: ["a"],
    properties: { a: { type: "integer" } },
  });
} catch (e) {
  const err = e as ValidationError;
  // err.path is like $[0].a
  throw err;
}
```

## Example 2: validate SQL against an allowlist (C++ / Python / TypeScript)

This example enforces a conservative policy:

- Only `SELECT` is allowed
- Comments and semicolons are forbidden
- A `LIMIT` is required and must not exceed a maximum
- Only a known set of tables can appear

SQL input:

```sql
SELECT u.id FROM users u WHERE u.id = ? LIMIT 1
```

### C++ (SQL)

```cpp
#include "llm_structured.hpp"

int main() {
  llm_structured::Json schema = llm_structured::loads_jsonish(R"JSON(
{
  "allowedStatements": ["select"],
  "forbidComments": true,
  "forbidSemicolon": true,
  "requireFrom": true,
  "requireWhere": true,
  "requireLimit": true,
  "maxLimit": 100,
  "forbidUnion": true,
  "forbidSubqueries": true,
  "allowedTables": ["users"],
  "placeholderStyle": "either"
}
)JSON");

  auto out = llm_structured::parse_and_validate_sql(
      "SELECT u.id FROM users u WHERE u.id = ? LIMIT 1",
      schema);
  (void)out;
}
```

### Python (SQL)

```python
from llm_structured import ValidationError, parse_and_validate_sql

schema = {
  "allowedStatements": ["select"],
  "forbidComments": True,
  "forbidSemicolon": True,
  "requireFrom": True,
  "requireWhere": True,
  "requireLimit": True,
  "maxLimit": 100,
  "forbidUnion": True,
  "forbidSubqueries": True,
  "allowedTables": ["users"],
  "placeholderStyle": "either",
}

try:
  out = parse_and_validate_sql("SELECT u.id FROM users u WHERE u.id = ? LIMIT 1", schema)
  print(out)
except ValidationError as e:
  raise
```

### TypeScript (SQL)

```ts
import { parseAndValidateSql, type ValidationError } from "./src/index";

const schema = {
  allowedStatements: ["select"],
  forbidComments: true,
  forbidSemicolon: true,
  requireFrom: true,
  requireWhere: true,
  requireLimit: true,
  maxLimit: 100,
  forbidUnion: true,
  forbidSubqueries: true,
  allowedTables: ["users"],
  placeholderStyle: "either",
} as const;

try {
  const out = parseAndValidateSql("SELECT u.id FROM users u WHERE u.id = ? LIMIT 1", schema);
  console.log(out);
} catch (e) {
  const err = e as ValidationError;
  throw err;
}
```

## CLI (C++)

After building the C++ targets:

### JSON

```powershell
echo '```json\n{"a":1,}\n```' | build/llm_structured_cpp/llm_structured_cli json
```

With a schema file:

```powershell
build/llm_structured_cpp/llm_structured_cli json --schema schema.json < input.txt
```

### SQL

```powershell
echo 'SELECT u.id FROM users u WHERE u.id = ? LIMIT 1' | build/llm_structured_cpp/llm_structured_cli sql --schema sql_schema.json
```

## Streaming

The streaming APIs are designed for chunked input:

- C++: `JsonStreamParser`, `JsonStreamCollector`, `JsonStreamBatchCollector`, `JsonStreamValidatedBatchCollector`, `SqlStreamParser`
- Python: `JsonStreamParser`, `JsonStreamCollector`, `JsonStreamBatchCollector`, `JsonStreamValidatedBatchCollector`, `SqlStreamParser`
- TypeScript: `new JsonStreamParser(...)`, `new JsonStreamCollector(...)`, `new JsonStreamBatchCollector(...)`, `new JsonStreamValidatedBatchCollector(...)`, `new SqlStreamParser(...)`

Core semantics:

- `append(chunk)`: feed more text
- `poll()`: returns either a value, an error, or not ready yet
- `finish()` / `close()`: signal no more input will arrive (parser vs collector)
- `location()`: best-effort position within the current internal buffer

## Schema support (high level)

The JSON validator intentionally supports a useful subset of JSON Schema. Common keywords include:

- `type`, `enum`, `const`
- `properties`, `required`, `additionalProperties`, `propertyNames`
- `items`, `minItems`, `maxItems`, `contains`, `minContains`, `maxContains`
- `minLength`, `maxLength`, `pattern`, `format`
- `minimum`, `maximum`, `multipleOf`
- `allOf`, `anyOf`, `oneOf`, `if` / `then` / `else`, `dependentRequired`

## Troubleshooting

- C++ builds require a C++17 toolchain and CMake.
- Python/TypeScript native builds require a working C/C++ build toolchain (for example, a Visual Studio build environment on Windows).
- If you see an import error for the Python native module, rebuild/install the editable package from `python/`.

## Developing / Contributing

This project is under active development — issues, suggestions, and code contributions are welcome.

- Open an issue for bugs, feature requests, or design discussion.
- Small PRs are easiest to review: keep changes focused and include/adjust tests.
- If you add or change public APIs, please update the docs（ if it has ） and keep C++/Python/TypeScript behavior consistent.
