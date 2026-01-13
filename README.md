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

### YAML-ish parsing

Parse YAML from LLM output with automatic repairs:

- Extract YAML from `\`\`\`yaml` or `\`\`\`yml` fenced blocks
- Extract multiple YAML documents (separated by `---`)
- Fix tabs → spaces and normalize indentation
- Allow inline JSON objects/arrays within YAML
- Validate against JSON Schema (same as JSON)

Repairs are configurable via `YamlRepairConfig`:

- `fixTabs`: convert tabs to spaces
- `normalizeIndentation`: ensure consistent spacing
- `fixUnquotedValues`: handle unquoted special characters
- `allowInlineJson`: permit JSON-style syntax within YAML
- `quoteAmbiguousStrings`: auto-quote strings that could be numbers/booleans

APIs mirror JSON-ish pattern:
- C++: `loads_yamlish`, `loads_yamlish_ex`, `parse_and_validate_yaml`, `dumps_yaml`
- Python: `loads_yamlish`, `parse_and_validate_yaml`, `dumps_yaml`
- TypeScript: `loadsYamlish`, `parseAndValidateYaml`, `dumpsYaml`

### TOML-ish parsing

Parse TOML from LLM output with automatic repairs:

- Extract TOML from `\`\`\`toml` fenced blocks
- Support for standard tables `[section]` and arrays of tables `[[items]]`
- Handle dotted keys (`a.b.c = value`)
- Parse inline tables and inline arrays
- Support for all TOML value types (strings, numbers, booleans, dates)
- Convert single quotes to double quotes
- Normalize whitespace (tabs to spaces)
- Validate against JSON Schema (same as JSON)

Repairs are configurable via `TomlRepairConfig`:

- `fixUnquotedStrings`: handle unquoted string values
- `allowSingleQuotes`: convert single quotes to double quotes
- `normalizeWhitespace`: convert tabs to spaces
- `fixTableNames`: auto-fix table names with special characters
- `allowMultilineInlineTables`: permit multiline inline tables

APIs mirror JSON-ish pattern:
- C++: `loads_tomlish`, `loads_tomlish_ex`, `parse_and_validate_toml`, `dumps_toml`
- Python: `loads_tomlish`, `parse_and_validate_toml`, `dumps_toml`
- TypeScript: `loadsTomlish`, `parseAndValidateToml`, `dumpsToml`

### XML / HTML parsing

Parse XML and HTML from LLM output with automatic repairs:

- Extract XML/HTML from `\`\`\`xml` or `\`\`\`html` fenced blocks
- Parse well-formed XML and lenient HTML (auto-close tags, unquoted attributes)
- Support for elements, text, comments, CDATA, processing instructions, and doctypes
- Query nodes with CSS-like selectors (`query_xml`)
- Convert XML to JSON representation (`xml_to_json`)
- Extract text content from node trees (`xml_text_content`)
- Validate against JSON Schema (same as JSON)

Repairs are configurable via `XmlRepairConfig`:

- `html_mode`: enable HTML-specific parsing (void elements, optional closing tags)
- `fix_unquoted_attributes`: handle `<div class=foo>` style attributes
- `auto_close_tags`: automatically close unclosed tags
- `normalize_whitespace`: normalize whitespace in text nodes
- `lowercase_names`: convert tag/attribute names to lowercase
- `decode_entities`: decode HTML entities (`&amp;` → `&`)

APIs mirror JSON-ish pattern:
- C++: `loads_xml`, `loads_xml_ex`, `loads_html`, `loads_html_ex`, `xml_to_json`, `dumps_xml`, `dumps_html`, `query_xml`, `validate_xml`, `parse_and_validate_xml`
- Python: `loads_xml`, `loads_xml_ex`, `loads_html`, `loads_html_ex`, `xml_to_json`, `dumps_xml`, `dumps_html`, `query_xml`, `validate_xml`, `parse_and_validate_xml`
- TypeScript: `loadsXml`, `loadsXmlEx`, `loadsHtml`, `loadsHtmlEx`, `xmlToJson`, `dumpsXml`, `dumpsHtml`, `queryXml`, `validateXml`, `parseAndValidateXml`

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

### Schema Inference

Automatically infer JSON Schema from example values:

- Infer type, format, and constraints from one or more sample values
- Detect common string formats: `date-time`, `date`, `time`, `email`, `uri`, `uuid`, `ipv4`, `hostname`
- Merge schemas from multiple examples (intersection of required fields, union of properties)
- Detect enums from repeated string values
- Configurable via `SchemaInferenceConfig`:

Options:

- `include_examples`: include sample values in `examples` array
- `max_examples`: maximum number of examples to collect (default: 5)
- `include_default`: include first seen value as `default`
- `infer_formats`: detect string formats (email, uri, date-time, etc.)
- `infer_patterns`: generate regex patterns from string values
- `infer_numeric_ranges`: add `minimum`/`maximum` from observed values
- `infer_string_lengths`: add `minLength`/`maxLength` from observed values
- `infer_array_lengths`: add `minItems`/`maxItems` from observed arrays
- `required_by_default`: mark all properties as required (default: true)
- `strict_additional_properties`: set `additionalProperties: false` (default: true)
- `prefer_integer`: use `integer` type for whole numbers (default: true)
- `allow_any_of`: use `anyOf` for mixed types (default: true)
- `detect_enums`: detect enum values from repeated strings (default: false)
- `max_enum_values`: max unique values to consider as enum (default: 10)

APIs:

- C++: `infer_schema`, `infer_schema_from_values`, `merge_schemas`
- Python: `infer_schema`, `infer_schema_from_values`, `merge_schemas`
- TypeScript: `inferSchema`, `inferSchemaFromValues`, `mergeSchemas`

**Works with YAML and TOML too:** Schema inference operates on parsed values, so you can infer schemas from YAML/TOML by parsing first:

```python
# YAML → Schema
yaml_value = loads_yamlish("name: Alice\nage: 30")
schema = infer_schema(yaml_value)

# TOML → Schema
toml_value = loads_tomlish('[user]\nname = "Alice"')
schema = infer_schema(toml_value)
```

**Note:** XML/HTML have a different structure (attributes, mixed content) and cannot be directly mapped to JSON Schema.

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

## Example 1c: parse + validate YAML from LLM output (C++ / Python / TypeScript)

The library also supports YAML parsing with automatic repairs for common LLM output issues:

- Extracts YAML from `\`\`\`yaml` fenced blocks
- Fixes tabs and mixed indentation
- Allows inline JSON within YAML
- Validates against JSON Schema

Input YAML:

````yaml
```yaml
users:
  - name: Alice
    age: 30
  - name: Bob
    age: 25
```
````

### C++ (YAML)

```cpp
#include "llm_structured.hpp"

int main() {
  llm_structured::Json schema = llm_structured::loads_jsonish(R"JSON(
{
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
          "age": {"type": "integer", "minimum": 0}
        }
      }
    }
  }
}
)JSON");

  const std::string fence_open = std::string("`") + "``yaml\n";
  const std::string fence_close = std::string("`") + "``\n";
  const std::string yaml_text =
      fence_open +
      "users:\n"
      "  - name: Alice\n"
      "    age: 30\n"
      "  - name: Bob\n"
      "    age: 25\n" +
      fence_close;

  try {
    auto obj = llm_structured::parse_and_validate_yaml(yaml_text, schema);
    
    // Serialize back to YAML
    std::string yaml_out = llm_structured::dumps_yaml(obj);
    (void)yaml_out;
  } catch (const llm_structured::ValidationError& e) {
    // e.kind / e.path
    throw;
  }
}
```

### Python (YAML)

```python
from llm_structured import parse_and_validate_yaml, dumps_yaml, ValidationError

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

yaml_text = """
```yaml
users:
  - name: Alice
    age: 30
  - name: Bob
    age: 25
```
"""

try:
    obj = parse_and_validate_yaml(yaml_text, schema)
    print(obj)
    
    # Serialize back to YAML
    yaml_out = dumps_yaml(obj)
    print(yaml_out)
except ValidationError as e:
    # e.kind / e.path / e.jsonPointer
    raise
```

### TypeScript (YAML)

```ts
import { parseAndValidateYaml, dumpsYaml, type ValidationError } from "./src/index";

const schema = {
  type: "object",
  required: ["users"],
  properties: {
    users: {
      type: "array",
      items: {
        type: "object",
        required: ["name", "age"],
        properties: {
          name: { type: "string" },
          age: { type: "integer", minimum: 0 },
        },
      },
    },
  },
} as const;

const yamlText = `
\`\`\`yaml
users:
  - name: Alice
    age: 30
  - name: Bob
    age: 25
\`\`\`
`;

try {
  const obj = parseAndValidateYaml(yamlText, schema);
  console.log(obj);
  
  // Serialize back to YAML
  const yamlOut = dumpsYaml(obj);
  console.log(yamlOut);
} catch (e) {
  const err = e as ValidationError;
  throw err;
}
```

## Example 1d: parse + validate XML/HTML from LLM output (C++ / Python / TypeScript)

The library supports XML and HTML parsing with automatic repairs:

- Extracts from `\`\`\`xml` or `\`\`\`html` fenced blocks
- Handles malformed HTML (unclosed tags, unquoted attributes)
- Query nodes with CSS-like selectors
- Validates against JSON Schema

Input XML:

````xml
```xml
<config>
  <server host="localhost" port="8080"/>
  <database>
    <connection>postgresql://localhost/db</connection>
  </database>
</config>
```
````

### C++ (XML)

```cpp
#include "llm_structured.hpp"

int main() {
  const std::string fence_open = std::string("`") + "``xml\n";
  const std::string fence_close = std::string("`") + "``\n";
  const std::string xml_text =
      fence_open +
      "<config>\n"
      "  <server host=\"localhost\" port=\"8080\"/>\n"
      "  <database>\n"
      "    <connection>postgresql://localhost/db</connection>\n"
      "  </database>\n"
      "</config>\n" +
      fence_close;

  // Parse XML
  auto root = llm_structured::loads_xml(xml_text);

  // Query nodes by tag name
  auto servers = llm_structured::query_xml(root, "server");

  // Get attribute
  std::string host = llm_structured::xml_get_attribute(servers[0], "host");

  // Get text content
  std::string text = llm_structured::xml_text_content(root);

  // Convert to JSON representation
  auto json = llm_structured::xml_to_json(root);

  // Serialize back to XML
  std::string xml_out = llm_structured::dumps_xml(root);

  return 0;
}
```

### Python (XML)

```python
from llm_structured import (
    loads_xml, loads_html, query_xml, xml_get_attribute,
    xml_text_content, xml_to_json, dumps_xml, dumps_html
)

xml_text = """
```xml
<config>
  <server host="localhost" port="8080"/>
  <database>
    <connection>postgresql://localhost/db</connection>
  </database>
</config>
```
"""

# Parse XML
result = loads_xml(xml_text)
if result["ok"]:
    root = result["root"]
    
    # Query nodes by tag name
    servers = query_xml(root, "server")
    print(f"Found {len(servers)} server(s)")
    
    # Get attribute
    host = xml_get_attribute(servers[0], "host")
    print(f"Host: {host}")
    
    # Get all text content
    text = xml_text_content(root)
    print(f"Text: {text}")
    
    # Convert to JSON representation
    json_repr = xml_to_json(xml_text)
    print(f"JSON: {json_repr}")
    
    # Serialize back to XML
    xml_out = dumps_xml(root)
    print(xml_out)

# Parse HTML with lenient mode
html_text = '<div class=container><p>Hello <b>World</b></div>'
result = loads_html(html_text)
if result["ok"]:
    html_out = dumps_html(result["root"])
    print(html_out)
```

### TypeScript (XML)

```ts
import {
  loadsXml, loadsHtml, queryXml, xmlGetAttribute,
  xmlTextContent, xmlToJson, dumpsXml, dumpsHtml
} from "./src/index";

const xmlText = `
\`\`\`xml
<config>
  <server host="localhost" port="8080"/>
  <database>
    <connection>postgresql://localhost/db</connection>
  </database>
</config>
\`\`\`
`;

// Parse XML
const result = loadsXml(xmlText);
if (result.ok) {
  const root = result.root;
  
  // Query nodes by tag name
  const servers = queryXml(root, "server");
  console.log(`Found ${servers.length} server(s)`);
  
  // Get attribute
  const host = xmlGetAttribute(servers[0], "host");
  console.log(`Host: ${host}`);
  
  // Get all text content
  const text = xmlTextContent(root);
  console.log(`Text: ${text}`);
  
  // Convert to JSON representation
  const jsonRepr = xmlToJson(xmlText);
  console.log("JSON:", jsonRepr);
  
  // Serialize back to XML
  const xmlOut = dumpsXml(root);
  console.log(xmlOut);
}

// Parse HTML with lenient mode
const htmlText = '<div class=container><p>Hello <b>World</b></div>';
const htmlResult = loadsHtml(htmlText);
if (htmlResult.ok) {
  const htmlOut = dumpsHtml(htmlResult.root);
  console.log(htmlOut);
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

## Example 3: Schema Inference (Python / TypeScript)

Automatically infer JSON Schema from example values.

### Python (Schema Inference)

```python
from llm_structured import infer_schema, infer_schema_from_values, merge_schemas
import json

# Infer schema from a single value
value = {
    "name": "Alice",
    "age": 30,
    "email": "alice@example.com",
    "created_at": "2024-01-15T10:30:00Z"
}

schema = infer_schema(value)
print(json.dumps(schema, indent=2))
# Output:
# {
#   "type": "object",
#   "properties": {
#     "name": {"type": "string"},
#     "age": {"type": "integer"},
#     "email": {"type": "string", "format": "email"},
#     "created_at": {"type": "string", "format": "date-time"}
#   },
#   "required": ["name", "age", "email", "created_at"],
#   "additionalProperties": false
# }

# Infer schema from multiple values (merges properties)
values = [
    {"name": "Alice", "age": 30},
    {"name": "Bob", "age": 25, "city": "NYC"},
    {"name": "Carol", "age": 35}
]

schema = infer_schema_from_values(values)
# Properties are merged; required = intersection of all examples

# Custom config
config = {
    "infer_formats": True,
    "infer_numeric_ranges": True,
    "include_examples": True,
    "max_examples": 3,
    "detect_enums": True,
    "max_enum_values": 10
}
schema = infer_schema(value, config)

# Merge two schemas
schema1 = {"type": "object", "properties": {"a": {"type": "string"}}}
schema2 = {"type": "object", "properties": {"b": {"type": "number"}}}
merged = merge_schemas(schema1, schema2)
```

### TypeScript (Schema Inference)

```ts
import { inferSchema, inferSchemaFromValues, mergeSchemas } from "./src/index";

// Infer schema from a single value
const value = {
  name: "Alice",
  age: 30,
  email: "alice@example.com",
  createdAt: "2024-01-15T10:30:00Z"
};

const schema = inferSchema(value);
console.log(JSON.stringify(schema, null, 2));

// Infer from multiple values
const values = [
  { name: "Alice", age: 30 },
  { name: "Bob", age: 25, city: "NYC" }
];

const mergedSchema = inferSchemaFromValues(values);

// Custom config
const config = {
  inferFormats: true,
  inferNumericRanges: true,
  includeExamples: true,
  detectEnums: true
};
const schemaWithConfig = inferSchema(value, config);

// Merge schemas
const schema1 = { type: "object", properties: { a: { type: "string" } } };
const schema2 = { type: "object", properties: { b: { type: "number" } } };
const merged = mergeSchemas(schema1, schema2);
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
