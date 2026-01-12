#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace llm_structured {

struct ValidationError : public std::runtime_error {
  std::string path;
  std::string message;
  std::string kind;  // schema | type | limit | parse
  explicit ValidationError(std::string message, std::string path_ = "$", std::string kind_ = "schema")
      : std::runtime_error(message), path(std::move(path_)), message(std::move(message)), kind(std::move(kind_)) {}

  const char* what() const noexcept override { return message.c_str(); }
};

// Best-effort conversion from a JSONPath-ish string like "$.a[0].b" to a JSON Pointer like "/a/0/b".
// Non-standard segments (e.g. "$.headings[Intro]") are preserved as a pointer segment ("/headings/Intro").
std::string json_pointer_from_path(const std::string& json_path);

struct Json;
using JsonObject = std::map<std::string, Json>;
using JsonArray = std::vector<Json>;

struct Json {
  using Value = std::variant<std::nullptr_t, bool, double, std::string, JsonArray, JsonObject>;
  Value value;

  Json() : value(nullptr) {}
  Json(std::nullptr_t) : value(nullptr) {}
  Json(bool b) : value(b) {}
  Json(double n) : value(n) {}
  Json(int64_t n) : value(static_cast<double>(n)) {}
  Json(std::string s) : value(std::move(s)) {}
  Json(const char* s) : value(std::string(s)) {}
  Json(JsonArray a) : value(std::move(a)) {}
  Json(JsonObject o) : value(std::move(o)) {}

  bool is_null() const;
  bool is_bool() const;
  bool is_number() const;
  bool is_string() const;
  bool is_array() const;
  bool is_object() const;

  const bool& as_bool() const;
  const double& as_number() const;
  const std::string& as_string() const;
  const JsonArray& as_array() const;
  const JsonObject& as_object() const;

  JsonArray& as_array();
  JsonObject& as_object();
};

// ---------------- JSON-ish ----------------

// Extracts a JSON candidate from LLM text (```json fenced block or first balanced {...} / [...] )
std::string extract_json_candidate(const std::string& text);

// Extracts ALL JSON candidates from text:
// - each ```json fenced block body
// - each balanced {...} or [...] outside fenced regions
std::vector<std::string> extract_json_candidates(const std::string& text);

struct RepairConfig {
  // Most users want best-effort parsing; set flags to false to make parsing stricter.
  bool fix_smart_quotes{true};
  bool strip_json_comments{true};
  bool replace_python_literals{true};
  bool convert_kv_object_to_json{true};
  bool quote_unquoted_keys{true};
  bool drop_trailing_commas{true};
  // Strictness toggle: the underlying parser supports single quotes; you can forbid them.
  bool allow_single_quotes{true};

  enum class DuplicateKeyPolicy {
    Error,
    FirstWins,
    LastWins,
  };

  // How to handle duplicate keys inside JSON objects.
  // Default: FirstWins (matches historical behavior).
  DuplicateKeyPolicy duplicate_key_policy{DuplicateKeyPolicy::FirstWins};
};

struct RepairMetadata {
  bool extracted_from_fence{false};
  bool fixed_smart_quotes{false};
  bool stripped_comments{false};
  bool replaced_python_literals{false};
  bool converted_kv_object{false};
  bool quoted_unquoted_keys{false};
  bool dropped_trailing_commas{false};

  // Number of duplicate keys encountered while parsing objects.
  int duplicateKeyCount{0};

  // Which duplicate key policy was applied during parsing.
  RepairConfig::DuplicateKeyPolicy duplicateKeyPolicy{RepairConfig::DuplicateKeyPolicy::FirstWins};
};

struct JsonishParseResult {
  Json value;
  std::string fixed;
  RepairMetadata metadata;
};

struct JsonishParseAllResult {
  JsonArray values;
  std::vector<std::string> fixed;
  std::vector<RepairMetadata> metadata;
};

// Applies lightweight "LLM JSON" repairs (controlled by RepairConfig) and parses.
Json loads_jsonish(const std::string& text);

// Like loads_jsonish(), but returns repair metadata and the fixed JSON-ish text.
JsonishParseResult loads_jsonish_ex(const std::string& text, const RepairConfig& repair = RepairConfig{});

// Parses all JSON candidates from the text and returns them as an array.
JsonArray loads_jsonish_all(const std::string& text);

// Like loads_jsonish_all(), but returns per-item fixed text and repair metadata.
JsonishParseAllResult loads_jsonish_all_ex(const std::string& text, const RepairConfig& repair = RepairConfig{});

// Validate a Json value against the repo's pragmatic JSON-schema subset.
void validate(const Json& value, const Json& schema, const std::string& path = "$");

// Collect-all variant: returns a list of validation failures (empty means valid).
std::vector<ValidationError> validate_all(const Json& value, const Json& schema, const std::string& path = "$");

// Convenience: parse candidate from text, parse jsonish, then validate.
Json parse_and_validate(const std::string& text, const Json& schema);

// Like parse_and_validate(), but returns repair metadata and the fixed JSON-ish text.
JsonishParseResult parse_and_validate_ex(const std::string& text, const Json& schema, const RepairConfig& repair = RepairConfig{});

// Parse and validate ALL JSON candidates; returns values as an array.
JsonArray parse_and_validate_all(const std::string& text, const Json& schema);

// Like parse_and_validate_all(), but returns per-item fixed text and repair metadata.
JsonishParseAllResult parse_and_validate_all_ex(const std::string& text, const Json& schema, const RepairConfig& repair = RepairConfig{});

// Like parse_and_validate(), but fills schema defaults before validating.
Json parse_and_validate_with_defaults(const std::string& text, const Json& schema);

// Like parse_and_validate_with_defaults(), but returns repair metadata and the fixed JSON-ish text.
JsonishParseResult parse_and_validate_with_defaults_ex(const std::string& text, const Json& schema, const RepairConfig& repair = RepairConfig{});

std::string dumps_json(const Json& value);

// ---------------- Markdown ----------------

struct MarkdownHeading {
  int level;
  std::string title;
  int line;
};

struct MarkdownCodeBlock {
  std::string lang;
  std::string body;
};

struct MarkdownTable {
  int startLine;
  std::string raw;
};

struct MarkdownParsed {
  std::string text;
  std::vector<std::string> lines;
  std::vector<MarkdownHeading> headings;
  std::map<std::string, std::vector<std::string>> sections;
  std::vector<MarkdownCodeBlock> codeBlocks;
  std::vector<int> bulletLineNumbers;
  std::vector<int> taskLineNumbers;
  std::vector<MarkdownTable> tables;
};

MarkdownParsed parse_markdown(const std::string& text);
void validate_markdown(const MarkdownParsed& parsed, const Json& schema);
MarkdownParsed parse_and_validate_markdown(const std::string& text, const Json& schema);

// ---------------- Key-Value (.env-ish) ----------------

using KeyValue = std::map<std::string, std::string>;

KeyValue loads_kv(const std::string& text);
void validate_kv(const KeyValue& kv, const Json& schema);
KeyValue parse_and_validate_kv(const std::string& text, const Json& schema);

// ---------------- SQL safety (heuristic) ----------------

struct SqlParsed {
  std::string sql;
  std::string statementType;
  bool hasWhere{false};
  bool hasFrom{false};
  bool hasLimit{false};
  std::optional<int> limit;
  bool hasUnion{false};
  bool hasComments{false};
  bool hasSubquery{false};
  std::vector<std::string> tables;
};

std::string extract_sql_candidate(const std::string& text);
SqlParsed parse_sql(const std::string& text);
void validate_sql(const SqlParsed& parsed, const Json& schema);
SqlParsed parse_and_validate_sql(const std::string& text, const Json& schema);

// ---------------- Streaming incremental parsing ----------------

template <typename T>
struct StreamOutcome {
  bool done{false};
  bool ok{false};
  std::optional<T> value;
  std::optional<ValidationError> error;
};

struct StreamLocation {
  size_t offset{0};  // byte offset within the current internal buffer
  int line{1};       // 1-based
  int col{1};        // 1-based
};

class JsonStreamParser {
 public:
  explicit JsonStreamParser(Json schema);
  JsonStreamParser(Json schema, size_t max_buffer_bytes);
  void reset();
  void finish();
  void append(const std::string& chunk);
  StreamOutcome<Json> poll();
  StreamLocation location() const;

 private:
  Json schema_;
  std::string buf_;
  size_t max_buffer_bytes_{0};
  bool finished_{false};
  bool done_{false};
  StreamOutcome<Json> last_{};
};

// Collects multiple JSON objects/arrays from a stream.
// - append(): feed more text
// - close(): signal no more data will arrive
// - poll(): returns {done:false} until close() or a validation error is hit.
// On success, poll() returns all parsed+validated items as a JsonArray.
class JsonStreamCollector {
 public:
  explicit JsonStreamCollector(Json item_schema);
  JsonStreamCollector(Json item_schema, size_t max_buffer_bytes, size_t max_items);
  void reset();
  void append(const std::string& chunk);
  void close();
  StreamOutcome<JsonArray> poll();
  StreamLocation location() const;

 private:
  Json schema_;
  std::string buf_;
  size_t max_buffer_bytes_{0};
  size_t max_items_{0};
  bool closed_{false};
  bool done_{false};
  JsonArray items_{};
  StreamOutcome<JsonArray> last_{};
};

// Like JsonStreamCollector, but emits items incrementally as soon as they can be parsed.
// poll() returns:
// - done=false, ok=false, value=null when no complete item is available yet
// - done=false, ok=true,  value=[...newItems] when one or more new items were parsed
// - done=true,  ok=true,  value=[...maybeEmpty] after close() once buffer is drained
// - done=true,  ok=false, error=... on first validation/parse failure
class JsonStreamBatchCollector {
 public:
  explicit JsonStreamBatchCollector(Json item_schema);
  JsonStreamBatchCollector(Json item_schema, size_t max_buffer_bytes, size_t max_items);
  void reset();
  void append(const std::string& chunk);
  void close();
  StreamOutcome<JsonArray> poll();
  StreamLocation location() const;

 private:
  Json schema_;
  std::string buf_;
  size_t max_buffer_bytes_{0};
  size_t max_items_{0};
  size_t emitted_items_{0};
  bool closed_{false};
  bool done_{false};
  StreamOutcome<JsonArray> last_{};
};

// Like JsonStreamBatchCollector, but applies schema defaults per item before validating.
class JsonStreamValidatedBatchCollector {
 public:
  explicit JsonStreamValidatedBatchCollector(Json item_schema);
  JsonStreamValidatedBatchCollector(Json item_schema, size_t max_buffer_bytes, size_t max_items);
  void reset();
  void append(const std::string& chunk);
  void close();
  StreamOutcome<JsonArray> poll();
  StreamLocation location() const;

 private:
  Json schema_;
  std::string buf_;
  size_t max_buffer_bytes_{0};
  size_t max_items_{0};
  size_t emitted_items_{0};
  bool closed_{false};
  bool done_{false};
  StreamOutcome<JsonArray> last_{};
};

class SqlStreamParser {
 public:
  explicit SqlStreamParser(Json schema);
  SqlStreamParser(Json schema, size_t max_buffer_bytes);
  void reset();
  void finish();
  void append(const std::string& chunk);
  StreamOutcome<SqlParsed> poll();
  StreamLocation location() const;

 private:
  Json schema_;
  std::string buf_;
  size_t max_buffer_bytes_{0};
  bool finished_{false};
  bool done_{false};
  StreamOutcome<SqlParsed> last_{};
};

}  // namespace llm_structured
