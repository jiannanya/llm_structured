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

// ---------------- Validation Repair Suggestions ----------------

// Represents a single repair suggestion for a validation error
struct RepairSuggestion {
  std::string path;           // JSONPath where the error occurred
  std::string error_kind;     // Type of error: type | required | enum | range | length | format | extra
  std::string message;        // Human-readable description of the problem
  std::string suggestion;     // Human-readable suggestion for fixing
  Json original_value;        // The original value that failed validation
  Json suggested_value;       // The suggested repaired value (may be null if no auto-fix possible)
  bool auto_fixable{false};   // Whether this can be automatically fixed
};

// Configuration for validation repair behavior
struct ValidationRepairConfig {
  bool coerce_types{true};           // Convert "123" to 123, "true" to true, etc.
  bool use_defaults{true};           // Fill missing required fields with schema defaults
  bool clamp_numbers{true};          // Clamp numbers to min/max bounds
  bool truncate_strings{false};      // Truncate strings exceeding maxLength
  bool truncate_arrays{false};       // Truncate arrays exceeding maxItems
  bool remove_extra_properties{true}; // Remove properties not in schema (when additionalProperties=false)
  bool fix_enums{true};              // Suggest closest enum value (Levenshtein distance)
  bool fix_formats{true};            // Attempt to fix string formats (dates, emails, etc.)
  int max_suggestions{50};           // Maximum number of suggestions to return
};

// Result of validation with repair suggestions
struct ValidationRepairResult {
  bool valid{false};                      // Whether original value was valid
  Json repaired_value;                    // The repaired value (original if valid, repaired if fixable)
  std::vector<RepairSuggestion> suggestions; // List of repair suggestions
  std::vector<ValidationError> unfixable_errors; // Errors that couldn't be auto-fixed
  bool fully_repaired{false};             // Whether all errors were auto-fixed
};

// Validate and return repair suggestions for any errors
ValidationRepairResult validate_with_repair(
    const Json& value, 
    const Json& schema, 
    const ValidationRepairConfig& config = ValidationRepairConfig{});

// Parse, validate, and repair in one step
ValidationRepairResult parse_and_repair(
    const std::string& text, 
    const Json& schema,
    const ValidationRepairConfig& config = ValidationRepairConfig{},
    const RepairConfig& parse_repair = RepairConfig{});

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

// ---------------- YAML-ish ----------------

// Configuration for YAML repairs.
struct YamlRepairConfig {
  // Convert tabs to spaces for consistent indentation.
  bool fix_tabs{true};
  // Normalize mixed indentation (use 2-space indentation).
  bool normalize_indentation{true};
  // Fix common YAML-ish issues: unquoted special chars, trailing whitespace.
  bool fix_unquoted_values{true};
  // Allow JSON-style inline objects/arrays within YAML.
  bool allow_inline_json{true};
  // Auto-quote strings that look like booleans or numbers but should be strings.
  bool quote_ambiguous_strings{false};
};

struct YamlRepairMetadata {
  bool extracted_from_fence{false};
  bool fixed_tabs{false};
  bool normalized_indentation{false};
  bool fixed_unquoted_values{false};
  bool converted_inline_json{false};
  bool quoted_ambiguous_strings{false};
};

struct YamlishParseResult {
  Json value;
  std::string fixed;
  YamlRepairMetadata metadata;
};

struct YamlishParseAllResult {
  JsonArray values;
  std::vector<std::string> fixed;
  std::vector<YamlRepairMetadata> metadata;
};

// Extract a YAML candidate from LLM text (```yaml fenced block or structured YAML-like content).
std::string extract_yaml_candidate(const std::string& text);

// Extract ALL YAML candidates from text (multiple ```yaml fences or YAML documents separated by ---).
std::vector<std::string> extract_yaml_candidates(const std::string& text);

// Parse YAML-ish text into a Json value (applies best-effort repairs).
Json loads_yamlish(const std::string& text);

// Like loads_yamlish(), but returns repair metadata and the fixed YAML-ish text.
YamlishParseResult loads_yamlish_ex(const std::string& text, const YamlRepairConfig& repair = YamlRepairConfig{});

// Parse all YAML documents from the text and return them as an array.
JsonArray loads_yamlish_all(const std::string& text);

// Like loads_yamlish_all(), but returns per-item fixed text and repair metadata.
YamlishParseAllResult loads_yamlish_all_ex(const std::string& text, const YamlRepairConfig& repair = YamlRepairConfig{});

// Convenience: parse candidate from text, parse yamlish, then validate against JSON schema.
Json parse_and_validate_yaml(const std::string& text, const Json& schema);

// Like parse_and_validate_yaml(), but returns repair metadata and the fixed YAML-ish text.
YamlishParseResult parse_and_validate_yaml_ex(const std::string& text, const Json& schema, const YamlRepairConfig& repair = YamlRepairConfig{});

// Parse and validate ALL YAML documents; returns values as an array.
JsonArray parse_and_validate_yaml_all(const std::string& text, const Json& schema);

// Like parse_and_validate_yaml_all(), but returns per-item fixed text and repair metadata.
YamlishParseAllResult parse_and_validate_yaml_all_ex(const std::string& text, const Json& schema, const YamlRepairConfig& repair = YamlRepairConfig{});

// Serialize Json value to YAML string.
std::string dumps_yaml(const Json& value, int indent = 2);

// ---------------- TOML-ish ----------------

// Configuration for TOML repairs.
struct TomlRepairConfig {
  // Fix common TOML-ish issues: unquoted strings, trailing commas.
  bool fix_unquoted_strings{true};
  // Allow single quotes (TOML normally only allows double quotes for basic strings).
  bool allow_single_quotes{true};
  // Convert tabs to spaces in multiline strings.
  bool normalize_whitespace{true};
  // Auto-fix missing quotes around table names with special chars.
  bool fix_table_names{true};
  // Allow inline tables to span multiple lines (not standard TOML).
  bool allow_multiline_inline_tables{true};
};

struct TomlRepairMetadata {
  bool extracted_from_fence{false};
  bool fixed_unquoted_strings{false};
  bool converted_single_quotes{false};
  bool normalized_whitespace{false};
  bool fixed_table_names{false};
  bool converted_multiline_inline{false};
};

struct TomlishParseResult {
  Json value;
  std::string fixed;
  TomlRepairMetadata metadata;
};

struct TomlishParseAllResult {
  JsonArray values;
  std::vector<std::string> fixed;
  std::vector<TomlRepairMetadata> metadata;
};

// Extract a TOML candidate from LLM text (```toml fenced block or structured TOML-like content).
std::string extract_toml_candidate(const std::string& text);

// Extract ALL TOML candidates from text (multiple ```toml fences).
std::vector<std::string> extract_toml_candidates(const std::string& text);

// Parse TOML-ish text into a Json value (applies best-effort repairs).
Json loads_tomlish(const std::string& text);

// Like loads_tomlish(), but returns repair metadata and the fixed TOML-ish text.
TomlishParseResult loads_tomlish_ex(const std::string& text, const TomlRepairConfig& repair = TomlRepairConfig{});

// Parse all TOML documents from the text and return them as an array.
JsonArray loads_tomlish_all(const std::string& text);

// Like loads_tomlish_all(), but returns per-item fixed text and repair metadata.
TomlishParseAllResult loads_tomlish_all_ex(const std::string& text, const TomlRepairConfig& repair = TomlRepairConfig{});

// Convenience: parse candidate from text, parse tomlish, then validate against JSON schema.
Json parse_and_validate_toml(const std::string& text, const Json& schema);

// Like parse_and_validate_toml(), but returns repair metadata and the fixed TOML-ish text.
TomlishParseResult parse_and_validate_toml_ex(const std::string& text, const Json& schema, const TomlRepairConfig& repair = TomlRepairConfig{});

// Parse and validate ALL TOML documents; returns values as an array.
JsonArray parse_and_validate_toml_all(const std::string& text, const Json& schema);

// Like parse_and_validate_toml_all(), but returns per-item fixed text and repair metadata.
TomlishParseAllResult parse_and_validate_toml_all_ex(const std::string& text, const Json& schema, const TomlRepairConfig& repair = TomlRepairConfig{});

// Serialize Json value to TOML string.
std::string dumps_toml(const Json& value);

// ---------------- XML/HTML-ish ----------------

// Represents an XML/HTML node (element, text, comment, etc.)
struct XmlNode {
  enum class Type { Element, Text, Comment, CData, ProcessingInstruction, Doctype };
  Type type{Type::Element};
  std::string name;           // Tag name for elements, target for PI
  std::string text;           // Text content for Text/Comment/CData/PI
  std::map<std::string, std::string> attributes;
  std::vector<XmlNode> children;
  bool self_closing{false};
};

// Configuration for XML/HTML repairs.
struct XmlRepairConfig {
  // Parse as HTML (more lenient: void elements, optional closing tags).
  bool html_mode{false};
  // Fix common issues: unquoted attributes, missing closing tags.
  bool fix_unquoted_attributes{true};
  // Auto-close unclosed tags.
  bool auto_close_tags{true};
  // Normalize whitespace in text nodes.
  bool normalize_whitespace{false};
  // Convert to lowercase tag/attribute names (HTML convention).
  bool lowercase_names{false};
  // Decode HTML entities (&amp; -> &).
  bool decode_entities{true};
};

struct XmlRepairMetadata {
  bool extracted_from_fence{false};
  bool fixed_unquoted_attributes{false};
  bool auto_closed_tags{false};
  bool normalized_whitespace{false};
  bool lowercased_names{false};
  bool decoded_entities{false};
  int unclosed_tag_count{0};
};

struct XmlParseResult {
  XmlNode root;
  std::string fixed;
  XmlRepairMetadata metadata;
};

struct XmlParseAllResult {
  std::vector<XmlNode> roots;
  std::vector<std::string> fixed;
  std::vector<XmlRepairMetadata> metadata;
};

// Extract an XML/HTML candidate from LLM text (```xml/html fenced block or <tag>...</tag>).
std::string extract_xml_candidate(const std::string& text);

// Extract ALL XML/HTML candidates from text.
std::vector<std::string> extract_xml_candidates(const std::string& text);

// Parse XML/HTML-ish text into an XmlNode tree (applies best-effort repairs).
XmlNode loads_xml(const std::string& text);

// Like loads_xml(), but returns repair metadata and the fixed text.
XmlParseResult loads_xml_ex(const std::string& text, const XmlRepairConfig& repair = XmlRepairConfig{});

// Parse HTML text (shortcut for XML with html_mode=true).
XmlNode loads_html(const std::string& text);

// Like loads_html(), but returns repair metadata.
XmlParseResult loads_html_ex(const std::string& text, const XmlRepairConfig& repair = XmlRepairConfig{});

// Convert XmlNode tree to a Json representation.
Json xml_to_json(const XmlNode& node);

// Parse XML/HTML and convert to Json.
Json loads_xml_as_json(const std::string& text);
Json loads_html_as_json(const std::string& text);

// Serialize XmlNode tree back to XML/HTML string.
std::string dumps_xml(const XmlNode& node, int indent = 2);
std::string dumps_html(const XmlNode& node, int indent = 2);

// Query XML nodes using simple XPath-like expressions.
std::vector<XmlNode*> query_xml(XmlNode& root, const std::string& selector);
std::vector<const XmlNode*> query_xml(const XmlNode& root, const std::string& selector);

// Get text content from node and all descendants.
std::string xml_text_content(const XmlNode& node);

// Get attribute value (returns empty string if not found).
std::string xml_get_attribute(const XmlNode& node, const std::string& name);

// Validate XML structure against a schema (element names, required attributes, etc.).
void validate_xml(const XmlNode& node, const Json& schema, const std::string& path = "$");

// Parse and validate XML/HTML.
XmlNode parse_and_validate_xml(const std::string& text, const Json& schema);
XmlParseResult parse_and_validate_xml_ex(const std::string& text, const Json& schema, const XmlRepairConfig& repair = XmlRepairConfig{});

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

// ---------------- Schema Inference ----------------

// Configuration for schema inference behavior
struct SchemaInferenceConfig {
  // Include "examples" array with sample values (up to max_examples)
  bool include_examples{false};
  int max_examples{3};
  
  // Include "default" from the first seen value
  bool include_default{false};
  
  // Infer "format" for strings (e.g., "date-time", "email", "uri")
  bool infer_formats{true};
  
  // Infer "pattern" for strings that look like specific formats
  bool infer_patterns{false};
  
  // Infer numeric constraints (minimum, maximum) from seen values
  bool infer_numeric_ranges{false};
  
  // Infer string constraints (minLength, maxLength) from seen values
  bool infer_string_lengths{false};
  
  // Infer array constraints (minItems, maxItems) from seen values
  bool infer_array_lengths{false};
  
  // Make all object properties required by default
  bool required_by_default{true};
  
  // Set additionalProperties to false by default
  bool strict_additional_properties{true};
  
  // Prefer "integer" over "number" when all values are whole numbers
  bool prefer_integer{true};
  
  // Merge multiple types into anyOf when values have different types
  bool allow_any_of{true};
  
  // Include "description" placeholders for properties
  bool include_descriptions{false};
  
  // Detect enum values when all values are from a small set of strings
  bool detect_enums{true};
  int max_enum_values{10};
};

// Infer JSON Schema from a single JSON value
Json infer_schema(const Json& value, const SchemaInferenceConfig& config = SchemaInferenceConfig{});

// Infer JSON Schema from multiple JSON values (merges schemas)
Json infer_schema_from_values(const JsonArray& values, const SchemaInferenceConfig& config = SchemaInferenceConfig{});

// Merge two schemas into one that accepts values valid for either schema
Json merge_schemas(const Json& schema1, const Json& schema2, const SchemaInferenceConfig& config = SchemaInferenceConfig{});

// ---------------- Streaming parsers ----------------

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
