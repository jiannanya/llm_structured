#include <node_api.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <utility>

#include "llm_structured.hpp"

using llm_structured::Json;
using llm_structured::JsonObject;
using llm_structured::SqlParsed;
using llm_structured::ValidationError;

static void ThrowTypeError(napi_env env, const char* msg) { napi_throw_type_error(env, nullptr, msg); }

static napi_value MakeString(napi_env env, const std::string& s);
static napi_value ToNapi(napi_env env, const Json& v);

static bool TryParseStreamLimit(const ValidationError& e, std::string& kind, double& current, double& max) {
  if (e.path == "$.stream.maxBufferBytes") {
    kind = "maxBufferBytes";
  } else if (e.path == "$.stream.maxItems") {
    kind = "maxItems";
  } else {
    return false;
  }

  const std::string msg = e.what();
  const char* cur_key = (kind == "maxBufferBytes") ? "size=" : "items=";
  const auto cur_pos = msg.find(cur_key);
  const auto max_pos = msg.find("max=");
  if (cur_pos == std::string::npos || max_pos == std::string::npos) return false;

  try {
    const auto cur_start = cur_pos + std::strlen(cur_key);
    const auto cur_end = msg.find_first_of(",)", cur_start);
    const auto max_start = max_pos + std::strlen("max=");
    const auto max_end = msg.find_first_of(")", max_start);
    current = std::stod(msg.substr(cur_start, cur_end - cur_start));
    max = std::stod(msg.substr(max_start, max_end - max_start));
    return true;
  } catch (...) {
    return false;
  }
}

static void ThrowError(napi_env env, const std::string& msg) {
  napi_throw_error(env, nullptr, msg.c_str());
}

static void ThrowErrorWithKind(napi_env env, const std::string& msg, const std::string& kind) {
  napi_value message;
  napi_create_string_utf8(env, msg.c_str(), msg.size(), &message);

  napi_value err;
  napi_create_error(env, nullptr, message, &err);
  napi_set_named_property(env, err, "message", message);
  napi_set_named_property(env, err, "kind", MakeString(env, kind));
  napi_throw(env, err);
}

static void ThrowValidationError(napi_env env, const ValidationError& e) {
  napi_value msg;
  napi_create_string_utf8(env, e.what(), NAPI_AUTO_LENGTH, &msg);

  napi_value err;
  napi_create_error(env, nullptr, msg, &err);

  // Ensure `err.message` is present and readable.
  napi_set_named_property(env, err, "message", msg);

  napi_value name;
  napi_create_string_utf8(env, "ValidationError", NAPI_AUTO_LENGTH, &name);
  napi_set_named_property(env, err, "name", name);

  napi_value path;
  napi_create_string_utf8(env, e.path.c_str(), e.path.size(), &path);
  napi_set_named_property(env, err, "path", path);

  napi_value ptr;
  auto jp = llm_structured::json_pointer_from_path(e.path);
  napi_create_string_utf8(env, jp.c_str(), jp.size(), &ptr);
  napi_set_named_property(env, err, "jsonPointer", ptr);

  napi_set_named_property(env, err, "kind", MakeString(env, e.kind));

  std::string kind;
  double current = 0;
  double max = 0;
  if (TryParseStreamLimit(e, kind, current, max)) {
    napi_value lim;
    napi_create_object(env, &lim);
    napi_set_named_property(env, lim, "kind", MakeString(env, kind));
    napi_value n;
    napi_create_double(env, current, &n);
    napi_set_named_property(env, lim, "current", n);
    napi_create_double(env, max, &n);
    napi_set_named_property(env, lim, "max", n);
    napi_set_named_property(env, err, "limit", lim);
  }

  napi_throw(env, err);
}

static bool GetStringUtf8(napi_env env, napi_value v, std::string& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_string) return false;

  size_t len = 0;
  if (napi_get_value_string_utf8(env, v, nullptr, 0, &len) != napi_ok) return false;

  out.resize(len);
  size_t written = 0;
  if (napi_get_value_string_utf8(env, v, out.data(), out.size() + 1, &written) != napi_ok) return false;
  out.resize(written);
  return true;
}

static bool GetOptionalSizeTProperty(napi_env env, napi_value obj, const char* key, size_t& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;

  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_number) {
    ThrowTypeError(env, "stream limits must be numbers");
    return false;
  }

  double d = 0;
  if (napi_get_value_double(env, v, &d) != napi_ok) return false;
  if (d < 0) {
    ThrowTypeError(env, "stream limits must be >= 0");
    return false;
  }
  out = static_cast<size_t>(d);
  return true;
}

static bool GetOptionalInt32Property(napi_env env, napi_value obj, const char* key, int32_t& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;

  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_number) {
    ThrowTypeError(env, "config fields must be numbers");
    return false;
  }

  int32_t n = 0;
  if (napi_get_value_int32(env, v, &n) != napi_ok) return false;
  out = n;
  return true;
}

static bool GetOptionalBoolProperty(napi_env env, napi_value obj, const char* key, bool& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_boolean) {
    ThrowTypeError(env, "repair config fields must be booleans");
    return false;
  }
  bool b = false;
  if (napi_get_value_bool(env, v, &b) != napi_ok) return false;
  out = b;
  return true;
}

static bool GetOptionalStringProperty(napi_env env, napi_value obj, const char* key, std::string& out) {
  bool has = false;
  if (napi_has_named_property(env, obj, key, &has) != napi_ok) return false;
  if (!has) return true;

  napi_value v;
  if (napi_get_named_property(env, obj, key, &v) != napi_ok) return false;

  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_string) {
    ThrowTypeError(env, "repair config fields must be booleans (or duplicateKeyPolicy as string)");
    return false;
  }
  return GetStringUtf8(env, v, out);
}

static bool RepairConfigFromNapi(napi_env env, napi_value v, llm_structured::RepairConfig& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t == napi_null || t == napi_undefined) return true;
  if (t != napi_object) {
    ThrowTypeError(env, "repair config must be an object");
    return false;
  }
  if (!GetOptionalBoolProperty(env, v, "fixSmartQuotes", out.fix_smart_quotes)) return false;
  if (!GetOptionalBoolProperty(env, v, "stripJsonComments", out.strip_json_comments)) return false;
  if (!GetOptionalBoolProperty(env, v, "replacePythonLiterals", out.replace_python_literals)) return false;
  if (!GetOptionalBoolProperty(env, v, "convertKvObjectToJson", out.convert_kv_object_to_json)) return false;
  if (!GetOptionalBoolProperty(env, v, "quoteUnquotedKeys", out.quote_unquoted_keys)) return false;
  if (!GetOptionalBoolProperty(env, v, "dropTrailingCommas", out.drop_trailing_commas)) return false;
  if (!GetOptionalBoolProperty(env, v, "allowSingleQuotes", out.allow_single_quotes)) return false;

  std::string pol;
  if (!GetOptionalStringProperty(env, v, "duplicateKeyPolicy", pol)) return false;
  if (!pol.empty()) {
    std::string s = pol;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (s == "error") {
      out.duplicate_key_policy = llm_structured::RepairConfig::DuplicateKeyPolicy::Error;
    } else if (s == "firstwins" || s == "first_wins" || s == "first") {
      out.duplicate_key_policy = llm_structured::RepairConfig::DuplicateKeyPolicy::FirstWins;
    } else if (s == "lastwins" || s == "last_wins" || s == "last") {
      out.duplicate_key_policy = llm_structured::RepairConfig::DuplicateKeyPolicy::LastWins;
    } else {
      ThrowTypeError(env, "duplicateKeyPolicy must be one of: error | firstWins | lastWins");
      return false;
    }
  }
  return true;
}

static bool ValidationRepairConfigFromNapi(napi_env env, napi_value v, llm_structured::ValidationRepairConfig& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t == napi_null || t == napi_undefined) return true;
  if (t != napi_object) {
    ThrowTypeError(env, "validation repair config must be an object");
    return false;
  }

  if (!GetOptionalBoolProperty(env, v, "coerceTypes", out.coerce_types)) return false;
  if (!GetOptionalBoolProperty(env, v, "useDefaults", out.use_defaults)) return false;
  if (!GetOptionalBoolProperty(env, v, "clampNumbers", out.clamp_numbers)) return false;
  if (!GetOptionalBoolProperty(env, v, "truncateStrings", out.truncate_strings)) return false;
  if (!GetOptionalBoolProperty(env, v, "truncateArrays", out.truncate_arrays)) return false;
  if (!GetOptionalBoolProperty(env, v, "removeExtraProperties", out.remove_extra_properties)) return false;
  if (!GetOptionalBoolProperty(env, v, "fixEnums", out.fix_enums)) return false;
  if (!GetOptionalBoolProperty(env, v, "fixFormats", out.fix_formats)) return false;

  // Also accept snake_case (Python-style) for convenience.
  if (!GetOptionalBoolProperty(env, v, "coerce_types", out.coerce_types)) return false;
  if (!GetOptionalBoolProperty(env, v, "use_defaults", out.use_defaults)) return false;
  if (!GetOptionalBoolProperty(env, v, "clamp_numbers", out.clamp_numbers)) return false;
  if (!GetOptionalBoolProperty(env, v, "truncate_strings", out.truncate_strings)) return false;
  if (!GetOptionalBoolProperty(env, v, "truncate_arrays", out.truncate_arrays)) return false;
  if (!GetOptionalBoolProperty(env, v, "remove_extra_properties", out.remove_extra_properties)) return false;
  if (!GetOptionalBoolProperty(env, v, "fix_enums", out.fix_enums)) return false;
  if (!GetOptionalBoolProperty(env, v, "fix_formats", out.fix_formats)) return false;

  int32_t max_suggestions = out.max_suggestions;
  if (!GetOptionalInt32Property(env, v, "maxSuggestions", max_suggestions)) return false;
  if (!GetOptionalInt32Property(env, v, "max_suggestions", max_suggestions)) return false;
  if (max_suggestions < 0) max_suggestions = 0;
  out.max_suggestions = static_cast<int>(max_suggestions);
  return true;
}

static napi_value RepairSuggestionToNapi(napi_env env, const llm_structured::RepairSuggestion& s) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_set_named_property(env, obj, "path", MakeString(env, s.path));
  napi_set_named_property(env, obj, "errorKind", MakeString(env, s.error_kind));
  napi_set_named_property(env, obj, "message", MakeString(env, s.message));
  napi_set_named_property(env, obj, "suggestion", MakeString(env, s.suggestion));
  napi_set_named_property(env, obj, "originalValue", ToNapi(env, s.original_value));
  napi_set_named_property(env, obj, "suggestedValue", ToNapi(env, s.suggested_value));
  napi_value b;
  napi_get_boolean(env, s.auto_fixable, &b);
  napi_set_named_property(env, obj, "autoFixable", b);
  return obj;
}

static napi_value RepairMetadataToNapi(napi_env env, const llm_structured::RepairMetadata& m) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value b;
  napi_get_boolean(env, m.extracted_from_fence, &b);
  napi_set_named_property(env, obj, "extractedFromFence", b);
  napi_get_boolean(env, m.fixed_smart_quotes, &b);
  napi_set_named_property(env, obj, "fixedSmartQuotes", b);
  napi_get_boolean(env, m.stripped_comments, &b);
  napi_set_named_property(env, obj, "strippedComments", b);
  napi_get_boolean(env, m.replaced_python_literals, &b);
  napi_set_named_property(env, obj, "replacedPythonLiterals", b);
  napi_get_boolean(env, m.converted_kv_object, &b);
  napi_set_named_property(env, obj, "convertedKvObject", b);
  napi_get_boolean(env, m.quoted_unquoted_keys, &b);
  napi_set_named_property(env, obj, "quotedUnquotedKeys", b);
  napi_get_boolean(env, m.dropped_trailing_commas, &b);
  napi_set_named_property(env, obj, "droppedTrailingCommas", b);

  napi_value n;
  napi_create_int32(env, m.duplicateKeyCount, &n);
  napi_set_named_property(env, obj, "duplicateKeyCount", n);

  const auto pol = m.duplicateKeyPolicy;
  std::string pols = "firstWins";
  if (pol == llm_structured::RepairConfig::DuplicateKeyPolicy::Error) pols = "error";
  if (pol == llm_structured::RepairConfig::DuplicateKeyPolicy::LastWins) pols = "lastWins";
  napi_set_named_property(env, obj, "duplicateKeyPolicy", MakeString(env, pols));
  return obj;
}

static napi_value StreamLocationToNapi(napi_env env, const llm_structured::StreamLocation& loc) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value n;
  napi_create_double(env, static_cast<double>(loc.offset), &n);
  napi_set_named_property(env, obj, "offset", n);
  napi_create_int32(env, loc.line, &n);
  napi_set_named_property(env, obj, "line", n);
  napi_create_int32(env, loc.col, &n);
  napi_set_named_property(env, obj, "col", n);
  return obj;
}

static napi_value MakeString(napi_env env, const std::string& s) {
  napi_value out;
  napi_create_string_utf8(env, s.c_str(), s.size(), &out);
  return out;
}

static napi_value ToNapi(napi_env env, const Json& v);

static bool FromNapi(napi_env env, napi_value v, Json& out);

static bool FromNapiObject(napi_env env, napi_value v, Json& out) {
  napi_value names;
  if (napi_get_property_names(env, v, &names) != napi_ok) return false;

  uint32_t len = 0;
  if (napi_get_array_length(env, names, &len) != napi_ok) return false;

  llm_structured::JsonObject obj;
  for (uint32_t i = 0; i < len; ++i) {
    napi_value keyv;
    if (napi_get_element(env, names, i, &keyv) != napi_ok) return false;
    std::string key;
    if (!GetStringUtf8(env, keyv, key)) return false;

    napi_value val;
    if (napi_get_property(env, v, keyv, &val) != napi_ok) return false;

    Json child;
    if (!FromNapi(env, val, child)) return false;
    obj.emplace(std::move(key), std::move(child));
  }
  out = Json(std::move(obj));
  return true;
}

static bool FromNapiArray(napi_env env, napi_value v, Json& out) {
  uint32_t len = 0;
  if (napi_get_array_length(env, v, &len) != napi_ok) return false;
  llm_structured::JsonArray arr;
  arr.reserve(len);
  for (uint32_t i = 0; i < len; ++i) {
    napi_value el;
    if (napi_get_element(env, v, i, &el) != napi_ok) return false;
    Json child;
    if (!FromNapi(env, el, child)) return false;
    arr.push_back(std::move(child));
  }
  out = Json(std::move(arr));
  return true;
}

static bool FromNapi(napi_env env, napi_value v, Json& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;

  if (t == napi_null || t == napi_undefined) {
    out = Json(nullptr);
    return true;
  }
  if (t == napi_boolean) {
    bool b = false;
    if (napi_get_value_bool(env, v, &b) != napi_ok) return false;
    out = Json(b);
    return true;
  }
  if (t == napi_number) {
    double n = 0;
    if (napi_get_value_double(env, v, &n) != napi_ok) return false;
    out = Json(n);
    return true;
  }
  if (t == napi_string) {
    std::string s;
    if (!GetStringUtf8(env, v, s)) return false;
    out = Json(std::move(s));
    return true;
  }
  if (t == napi_object) {
    bool is_array = false;
    if (napi_is_array(env, v, &is_array) != napi_ok) return false;
    if (is_array) return FromNapiArray(env, v, out);
    return FromNapiObject(env, v, out);
  }
  return false;
}

static napi_value ToNapiSqlParsed(napi_env env, const SqlParsed& parsed) {
  napi_value obj;
  napi_create_object(env, &obj);

  napi_set_named_property(env, obj, "sql", MakeString(env, parsed.sql));
  napi_set_named_property(env, obj, "statementType", MakeString(env, parsed.statementType));

  napi_value b;
  napi_get_boolean(env, parsed.hasWhere, &b);
  napi_set_named_property(env, obj, "hasWhere", b);
  napi_get_boolean(env, parsed.hasFrom, &b);
  napi_set_named_property(env, obj, "hasFrom", b);
  napi_get_boolean(env, parsed.hasLimit, &b);
  napi_set_named_property(env, obj, "hasLimit", b);
  napi_get_boolean(env, parsed.hasUnion, &b);
  napi_set_named_property(env, obj, "hasUnion", b);
  napi_get_boolean(env, parsed.hasComments, &b);
  napi_set_named_property(env, obj, "hasComments", b);
  napi_get_boolean(env, parsed.hasSubquery, &b);
  napi_set_named_property(env, obj, "hasSubquery", b);

  if (parsed.limit) {
    napi_value n;
    napi_create_int32(env, *parsed.limit, &n);
    napi_set_named_property(env, obj, "limit", n);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "limit", n);
  }

  napi_value tables;
  napi_create_array_with_length(env, parsed.tables.size(), &tables);
  for (size_t i = 0; i < parsed.tables.size(); ++i) {
    napi_set_element(env, tables, static_cast<uint32_t>(i), MakeString(env, parsed.tables[i]));
  }
  napi_set_named_property(env, obj, "tables", tables);
  return obj;
}

static napi_value MakeValidationErrorObject(napi_env env, const ValidationError& e) {
  napi_value obj;
  napi_create_object(env, &obj);

  napi_set_named_property(env, obj, "name", MakeString(env, "ValidationError"));
  napi_set_named_property(env, obj, "message", MakeString(env, e.what()));
  napi_set_named_property(env, obj, "path", MakeString(env, e.path));
  napi_set_named_property(env, obj, "kind", MakeString(env, e.kind));
  napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(e.path)));

  std::string kind;
  double current = 0;
  double max = 0;
  if (TryParseStreamLimit(e, kind, current, max)) {
    napi_value lim;
    napi_create_object(env, &lim);
    napi_set_named_property(env, lim, "kind", MakeString(env, kind));
    napi_value n;
    napi_create_double(env, current, &n);
    napi_set_named_property(env, lim, "current", n);
    napi_create_double(env, max, &n);
    napi_set_named_property(env, lim, "max", n);
    napi_set_named_property(env, obj, "limit", lim);
  }
  return obj;
}

template <typename T>
static bool GetExternalPtr(napi_env env, napi_value v, T*& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t != napi_external) return false;
  void* ptr = nullptr;
  if (napi_get_value_external(env, v, &ptr) != napi_ok) return false;
  out = static_cast<T*>(ptr);
  return out != nullptr;
}

static napi_value ToNapiObject(napi_env env, const llm_structured::JsonObject& o) {
  napi_value obj;
  napi_create_object(env, &obj);
  for (const auto& kv : o) {
    napi_value val = ToNapi(env, kv.second);
    napi_set_named_property(env, obj, kv.first.c_str(), val);
  }
  return obj;
}

static napi_value ToNapiArray(napi_env env, const llm_structured::JsonArray& a) {
  napi_value arr;
  napi_create_array_with_length(env, a.size(), &arr);
  for (size_t i = 0; i < a.size(); ++i) {
    napi_value val = ToNapi(env, a[i]);
    napi_set_element(env, arr, static_cast<uint32_t>(i), val);
  }
  return arr;
}

static napi_value ToNapi(napi_env env, const Json& v) {
  if (v.is_null()) {
    napi_value n;
    napi_get_null(env, &n);
    return n;
  }
  if (v.is_bool()) {
    napi_value b;
    napi_get_boolean(env, v.as_bool(), &b);
    return b;
  }
  if (v.is_number()) {
    napi_value n;
    napi_create_double(env, v.as_number(), &n);
    return n;
  }
  if (v.is_string()) {
    return MakeString(env, v.as_string());
  }
  if (v.is_array()) {
    return ToNapiArray(env, v.as_array());
  }
  return ToNapiObject(env, v.as_object());
}

static napi_value ParseAndValidateJson(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateJson(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJson(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json result = llm_structured::parse_and_validate(text, schema);
    return ToNapi(env, result);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonWithDefaults(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaults(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaults(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json result = llm_structured::parse_and_validate_with_defaults(text, schema);
    return ToNapi(env, result);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value LoadsJsonishEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsJsonishEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsJsonishEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 2) {
    if (!RepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_jsonish_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", RepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ExtractJsonCandidates(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "extractJsonCandidates(text) expects 1 argument");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractJsonCandidates(text) expects text as string");
    return nullptr;
  }

  try {
    const auto cands = llm_structured::extract_json_candidates(text);
    napi_value arr;
    napi_create_array_with_length(env, cands.size(), &arr);
    for (size_t i = 0; i < cands.size(); ++i) {
      napi_set_element(env, arr, static_cast<uint32_t>(i), MakeString(env, cands[i]));
    }
    return arr;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value LoadsJsonishAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsJsonishAllEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsJsonishAllEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 2) {
    if (!RepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_jsonish_all_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);

    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);

    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);

    napi_value meta;
    napi_create_array_with_length(env, r.metadata.size(), &meta);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, meta, static_cast<uint32_t>(i), RepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", meta);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateJsonAllEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonAllEx(text, schemaJson, repair?) expects strings for text and schemaJson");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 3) {
    if (!RepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_all_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);

    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);

    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);

    napi_value meta;
    napi_create_array_with_length(env, r.metadata.size(), &meta);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, meta, static_cast<uint32_t>(i), RepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", meta);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateJsonEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonEx(text, schemaJson, repair?) expects strings for text and schemaJson");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 3) {
    if (!RepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", RepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateJsonWithDefaultsEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaultsEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateJsonWithDefaultsEx(text, schemaJson, repair?) expects strings for text and schemaJson");
    return nullptr;
  }

  llm_structured::RepairConfig repair;
  if (argc >= 3) {
    if (!RepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_with_defaults_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", RepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ValidateAllJson(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "validateAllJson(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "validateAllJson(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json value = llm_structured::loads_jsonish(text);

    auto errs = llm_structured::validate_all(value, schema, "$");
    napi_value arr;
    napi_create_array_with_length(env, errs.size(), &arr);
    for (size_t i = 0; i < errs.size(); ++i) {
      napi_value obj;
      napi_create_object(env, &obj);
      napi_set_named_property(env, obj, "message", MakeString(env, errs[i].what()));
      napi_set_named_property(env, obj, "path", MakeString(env, errs[i].path));
      napi_set_named_property(env, obj, "kind", MakeString(env, errs[i].kind));
      napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(errs[i].path)));
      napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ValidateAllJsonValue(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "validateAllJsonValue(value, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "validateAllJsonValue(value, schemaJson) expects schemaJson as string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json value;
    if (!FromNapi(env, argv[0], value)) {
      ThrowTypeError(env, "validateAllJsonValue(value, schemaJson) received an unsupported JS value");
      return nullptr;
    }

    auto errs = llm_structured::validate_all(value, schema, "$");
    napi_value arr;
    napi_create_array_with_length(env, errs.size(), &arr);
    for (size_t i = 0; i < errs.size(); ++i) {
      napi_value obj;
      napi_create_object(env, &obj);
      napi_set_named_property(env, obj, "message", MakeString(env, errs[i].what()));
      napi_set_named_property(env, obj, "path", MakeString(env, errs[i].path));
      napi_set_named_property(env, obj, "kind", MakeString(env, errs[i].kind));
      napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(errs[i].path)));
      napi_set_element(env, arr, static_cast<uint32_t>(i), obj);
    }
    return arr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

// ---- Validation Repair Suggestions ----

static napi_value ValidateWithRepair(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2) {
    ThrowTypeError(env, "validateWithRepair(value, schemaJson, config?) expects at least 2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "validateWithRepair(value, schemaJson, config?) expects schemaJson as string");
    return nullptr;
  }

  llm_structured::ValidationRepairConfig cfg;
  if (argc >= 3) {
    if (!ValidationRepairConfigFromNapi(env, argv[2], cfg)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json value;
    if (!FromNapi(env, argv[0], value)) {
      ThrowTypeError(env, "validateWithRepair(value, schemaJson, config?) received an unsupported JS value");
      return nullptr;
    }

    const auto r = llm_structured::validate_with_repair(value, schema, cfg);

    napi_value out;
    napi_create_object(env, &out);
    napi_value b;
    napi_get_boolean(env, r.valid, &b);
    napi_set_named_property(env, out, "valid", b);
    napi_get_boolean(env, r.fully_repaired, &b);
    napi_set_named_property(env, out, "fullyRepaired", b);
    napi_set_named_property(env, out, "repairedValue", ToNapi(env, r.repaired_value));

    napi_value sugg;
    napi_create_array_with_length(env, r.suggestions.size(), &sugg);
    for (size_t i = 0; i < r.suggestions.size(); ++i) {
      napi_set_element(env, sugg, static_cast<uint32_t>(i), RepairSuggestionToNapi(env, r.suggestions[i]));
    }
    napi_set_named_property(env, out, "suggestions", sugg);

    napi_value errs;
    napi_create_array_with_length(env, r.unfixable_errors.size(), &errs);
    for (size_t i = 0; i < r.unfixable_errors.size(); ++i) {
      napi_value obj;
      napi_create_object(env, &obj);
      napi_set_named_property(env, obj, "message", MakeString(env, r.unfixable_errors[i].what()));
      napi_set_named_property(env, obj, "path", MakeString(env, r.unfixable_errors[i].path));
      napi_set_named_property(env, obj, "kind", MakeString(env, r.unfixable_errors[i].kind));
      napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(r.unfixable_errors[i].path)));
      napi_set_element(env, errs, static_cast<uint32_t>(i), obj);
    }
    napi_set_named_property(env, out, "unfixableErrors", errs);

    return out;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndRepair(napi_env env, napi_callback_info info) {
  size_t argc = 4;
  napi_value argv[4];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2) {
    ThrowTypeError(env, "parseAndRepair(text, schemaJson, config?, parseRepair?) expects at least 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndRepair(text, schemaJson, ...) expects text and schemaJson as strings");
    return nullptr;
  }

  llm_structured::ValidationRepairConfig cfg;
  if (argc >= 3) {
    if (!ValidationRepairConfigFromNapi(env, argv[2], cfg)) return nullptr;
  }

  llm_structured::RepairConfig parse_repair;
  if (argc >= 4) {
    if (!RepairConfigFromNapi(env, argv[3], parse_repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    const auto r = llm_structured::parse_and_repair(text, schema, cfg, parse_repair);

    napi_value out;
    napi_create_object(env, &out);
    napi_value b;
    napi_get_boolean(env, r.valid, &b);
    napi_set_named_property(env, out, "valid", b);
    napi_get_boolean(env, r.fully_repaired, &b);
    napi_set_named_property(env, out, "fullyRepaired", b);
    napi_set_named_property(env, out, "repairedValue", ToNapi(env, r.repaired_value));

    napi_value sugg;
    napi_create_array_with_length(env, r.suggestions.size(), &sugg);
    for (size_t i = 0; i < r.suggestions.size(); ++i) {
      napi_set_element(env, sugg, static_cast<uint32_t>(i), RepairSuggestionToNapi(env, r.suggestions[i]));
    }
    napi_set_named_property(env, out, "suggestions", sugg);

    napi_value errs;
    napi_create_array_with_length(env, r.unfixable_errors.size(), &errs);
    for (size_t i = 0; i < r.unfixable_errors.size(); ++i) {
      napi_value obj;
      napi_create_object(env, &obj);
      napi_set_named_property(env, obj, "message", MakeString(env, r.unfixable_errors[i].what()));
      napi_set_named_property(env, obj, "path", MakeString(env, r.unfixable_errors[i].path));
      napi_set_named_property(env, obj, "kind", MakeString(env, r.unfixable_errors[i].kind));
      napi_set_named_property(env, obj, "jsonPointer", MakeString(env, llm_structured::json_pointer_from_path(r.unfixable_errors[i].path)));
      napi_set_element(env, errs, static_cast<uint32_t>(i), obj);
    }
    napi_set_named_property(env, out, "unfixableErrors", errs);

    return out;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

// ---- YAML APIs ----

static napi_value ExtractYamlCandidate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "extractYamlCandidate(text) expects 1 argument");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractYamlCandidate(text) expects text as string");
    return nullptr;
  }

  try {
    const auto cand = llm_structured::extract_yaml_candidate(text);
    return MakeString(env, cand);
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ExtractYamlCandidates(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "extractYamlCandidates(text) expects 1 argument");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractYamlCandidates(text) expects text as string");
    return nullptr;
  }

  try {
    const auto cands = llm_structured::extract_yaml_candidates(text);
    napi_value arr;
    napi_create_array_with_length(env, cands.size(), &arr);
    for (size_t i = 0; i < cands.size(); ++i) {
      napi_set_element(env, arr, static_cast<uint32_t>(i), MakeString(env, cands[i]));
    }
    return arr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static bool YamlRepairConfigFromNapi(napi_env env, napi_value v, llm_structured::YamlRepairConfig& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t == napi_null || t == napi_undefined) return true;
  if (t != napi_object) {
    ThrowTypeError(env, "yaml repair config must be an object");
    return false;
  }
  if (!GetOptionalBoolProperty(env, v, "fixTabs", out.fix_tabs)) return false;
  if (!GetOptionalBoolProperty(env, v, "normalizeIndentation", out.normalize_indentation)) return false;
  if (!GetOptionalBoolProperty(env, v, "fixUnquotedValues", out.fix_unquoted_values)) return false;
  if (!GetOptionalBoolProperty(env, v, "allowInlineJson", out.allow_inline_json)) return false;
  if (!GetOptionalBoolProperty(env, v, "quoteAmbiguousStrings", out.quote_ambiguous_strings)) return false;
  return true;
}

static napi_value YamlRepairMetadataToNapi(napi_env env, const llm_structured::YamlRepairMetadata& m) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value b;
  napi_get_boolean(env, m.extracted_from_fence, &b);
  napi_set_named_property(env, obj, "extractedFromFence", b);
  napi_get_boolean(env, m.fixed_tabs, &b);
  napi_set_named_property(env, obj, "fixedTabs", b);
  napi_get_boolean(env, m.normalized_indentation, &b);
  napi_set_named_property(env, obj, "normalizedIndentation", b);
  napi_get_boolean(env, m.fixed_unquoted_values, &b);
  napi_set_named_property(env, obj, "fixedUnquotedValues", b);
  napi_get_boolean(env, m.converted_inline_json, &b);
  napi_set_named_property(env, obj, "convertedInlineJson", b);
  napi_get_boolean(env, m.quoted_ambiguous_strings, &b);
  napi_set_named_property(env, obj, "quotedAmbiguousStrings", b);
  return obj;
}

static napi_value LoadsYamlishEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsYamlishEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsYamlishEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::YamlRepairConfig repair;
  if (argc >= 2) {
    if (!YamlRepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_yamlish_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", YamlRepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value LoadsYamlishAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsYamlishAllEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsYamlishAllEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::YamlRepairConfig repair;
  if (argc >= 2) {
    if (!YamlRepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_yamlish_all_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    
    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);
    
    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);
    
    napi_value metadata;
    napi_create_array_with_length(env, r.metadata.size(), &metadata);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, metadata, static_cast<uint32_t>(i), YamlRepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", metadata);
    
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value DumpsYaml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "dumpsYaml(value, indent?) expects 1-2 arguments");
    return nullptr;
  }

  Json value;
  if (!FromNapi(env, argv[0], value)) {
    ThrowTypeError(env, "dumpsYaml(value, indent?) expects value to be JSON-serializable");
    return nullptr;
  }

  int indent = 2;
  if (argc >= 2) {
    napi_valuetype t;
    if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
    if (t == napi_number) {
      double d = 0;
      if (napi_get_value_double(env, argv[1], &d) != napi_ok) return nullptr;
      indent = static_cast<int>(d);
    }
  }

  try {
    std::string yaml = llm_structured::dumps_yaml(value, indent);
    return MakeString(env, yaml);
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value ParseAndValidateYaml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateYaml(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateYaml(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json result = llm_structured::parse_and_validate_yaml(text, schema);
    return ToNapi(env, result);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateYamlEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateYamlEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateYamlEx(text, schemaJson, repair?) expects strings");
    return nullptr;
  }

  llm_structured::YamlRepairConfig repair;
  if (argc >= 3) {
    if (!YamlRepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_yaml_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", YamlRepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateYamlAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateYamlAllEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateYamlAllEx(text, schemaJson, repair?) expects strings");
    return nullptr;
  }

  llm_structured::YamlRepairConfig repair;
  if (argc >= 3) {
    if (!YamlRepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_yaml_all_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    
    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);
    
    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);
    
    napi_value metadata;
    napi_create_array_with_length(env, r.metadata.size(), &metadata);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, metadata, static_cast<uint32_t>(i), YamlRepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", metadata);
    
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

// ---- TOML helpers ----

static bool TomlRepairConfigFromNapi(napi_env env, napi_value v, llm_structured::TomlRepairConfig& out) {
  napi_valuetype t;
  if (napi_typeof(env, v, &t) != napi_ok) return false;
  if (t == napi_null || t == napi_undefined) return true;
  if (t != napi_object) {
    ThrowTypeError(env, "toml repair config must be an object");
    return false;
  }
  if (!GetOptionalBoolProperty(env, v, "fixUnquotedStrings", out.fix_unquoted_strings)) return false;
  if (!GetOptionalBoolProperty(env, v, "allowSingleQuotes", out.allow_single_quotes)) return false;
  if (!GetOptionalBoolProperty(env, v, "normalizeWhitespace", out.normalize_whitespace)) return false;
  if (!GetOptionalBoolProperty(env, v, "fixTableNames", out.fix_table_names)) return false;
  if (!GetOptionalBoolProperty(env, v, "allowMultilineInlineTables", out.allow_multiline_inline_tables)) return false;
  return true;
}

static napi_value TomlRepairMetadataToNapi(napi_env env, const llm_structured::TomlRepairMetadata& m) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value b;
  napi_get_boolean(env, m.extracted_from_fence, &b);
  napi_set_named_property(env, obj, "extractedFromFence", b);
  napi_get_boolean(env, m.fixed_unquoted_strings, &b);
  napi_set_named_property(env, obj, "fixedUnquotedStrings", b);
  napi_get_boolean(env, m.converted_single_quotes, &b);
  napi_set_named_property(env, obj, "convertedSingleQuotes", b);
  napi_get_boolean(env, m.normalized_whitespace, &b);
  napi_set_named_property(env, obj, "normalizedWhitespace", b);
  napi_get_boolean(env, m.fixed_table_names, &b);
  napi_set_named_property(env, obj, "fixedTableNames", b);
  napi_get_boolean(env, m.converted_multiline_inline, &b);
  napi_set_named_property(env, obj, "convertedMultilineInline", b);
  return obj;
}

static napi_value ExtractTomlCandidate(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "extractTomlCandidate(text) expects 1 argument");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractTomlCandidate(text) expects text as string");
    return nullptr;
  }

  try {
    std::string result = llm_structured::extract_toml_candidate(text);
    return MakeString(env, result);
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value ExtractTomlCandidates(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "extractTomlCandidates(text) expects 1 argument");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractTomlCandidates(text) expects text as string");
    return nullptr;
  }

  try {
    auto results = llm_structured::extract_toml_candidates(text);
    napi_value arr;
    napi_create_array_with_length(env, results.size(), &arr);
    for (size_t i = 0; i < results.size(); ++i) {
      napi_set_element(env, arr, static_cast<uint32_t>(i), MakeString(env, results[i]));
    }
    return arr;
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value LoadsTomlishEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsTomlishEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsTomlishEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::TomlRepairConfig repair;
  if (argc >= 2) {
    if (!TomlRepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_tomlish_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", TomlRepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value LoadsTomlishAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsTomlishAllEx(text, repair?) expects 1-2 arguments");
    return nullptr;
  }

  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "loadsTomlishAllEx(text, repair?) expects text as string");
    return nullptr;
  }

  llm_structured::TomlRepairConfig repair;
  if (argc >= 2) {
    if (!TomlRepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }

  try {
    auto r = llm_structured::loads_tomlish_all_ex(text, repair);
    napi_value obj;
    napi_create_object(env, &obj);

    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);

    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);

    napi_value metadata;
    napi_create_array_with_length(env, r.metadata.size(), &metadata);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, metadata, static_cast<uint32_t>(i), TomlRepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", metadata);

    return obj;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value DumpsToml(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "dumpsToml(value) expects 1 argument");
    return nullptr;
  }

  Json value;
  if (!FromNapi(env, argv[0], value)) {
    ThrowTypeError(env, "dumpsToml(value) expects value to be JSON-serializable");
    return nullptr;
  }

  try {
    std::string toml = llm_structured::dumps_toml(value);
    return MakeString(env, toml);
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value ParseAndValidateToml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateToml(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateToml(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    Json result = llm_structured::parse_and_validate_toml(text, schema);
    return ToNapi(env, result);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateTomlEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateTomlEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateTomlEx(text, schemaJson, repair?) expects strings");
    return nullptr;
  }

  llm_structured::TomlRepairConfig repair;
  if (argc >= 3) {
    if (!TomlRepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_toml_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_set_named_property(env, obj, "value", ToNapi(env, r.value));
    napi_set_named_property(env, obj, "fixed", MakeString(env, r.fixed));
    napi_set_named_property(env, obj, "metadata", TomlRepairMetadataToNapi(env, r.metadata));
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateTomlAllEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateTomlAllEx(text, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateTomlAllEx(text, schemaJson, repair?) expects strings");
    return nullptr;
  }

  llm_structured::TomlRepairConfig repair;
  if (argc >= 3) {
    if (!TomlRepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto r = llm_structured::parse_and_validate_toml_all_ex(text, schema, repair);
    napi_value obj;
    napi_create_object(env, &obj);

    napi_value values;
    napi_create_array_with_length(env, r.values.size(), &values);
    for (size_t i = 0; i < r.values.size(); ++i) {
      napi_set_element(env, values, static_cast<uint32_t>(i), ToNapi(env, r.values[i]));
    }
    napi_set_named_property(env, obj, "values", values);

    napi_value fixed;
    napi_create_array_with_length(env, r.fixed.size(), &fixed);
    for (size_t i = 0; i < r.fixed.size(); ++i) {
      napi_set_element(env, fixed, static_cast<uint32_t>(i), MakeString(env, r.fixed[i]));
    }
    napi_set_named_property(env, obj, "fixed", fixed);

    napi_value metadata;
    napi_create_array_with_length(env, r.metadata.size(), &metadata);
    for (size_t i = 0; i < r.metadata.size(); ++i) {
      napi_set_element(env, metadata, static_cast<uint32_t>(i), TomlRepairMetadataToNapi(env, r.metadata[i]));
    }
    napi_set_named_property(env, obj, "metadata", metadata);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

// ---- XML / HTML helpers ----

static napi_value XmlNodeToNapi(napi_env env, const llm_structured::XmlNode& node);

static napi_value XmlNodeToNapi(napi_env env, const llm_structured::XmlNode& node) {
  napi_value obj;
  napi_create_object(env, &obj);

  const char* type_str = "element";
  switch (node.type) {
    case llm_structured::XmlNode::Type::Element: type_str = "element"; break;
    case llm_structured::XmlNode::Type::Text: type_str = "text"; break;
    case llm_structured::XmlNode::Type::Comment: type_str = "comment"; break;
    case llm_structured::XmlNode::Type::CData: type_str = "cdata"; break;
    case llm_structured::XmlNode::Type::ProcessingInstruction: type_str = "processing_instruction"; break;
    case llm_structured::XmlNode::Type::Doctype: type_str = "doctype"; break;
  }
  napi_set_named_property(env, obj, "type", MakeString(env, type_str));
  napi_set_named_property(env, obj, "name", MakeString(env, node.name));
  napi_set_named_property(env, obj, "text", MakeString(env, node.text));

  napi_value attrs;
  napi_create_object(env, &attrs);
  for (const auto& kv : node.attributes) {
    napi_set_named_property(env, attrs, kv.first.c_str(), MakeString(env, kv.second));
  }
  napi_set_named_property(env, obj, "attributes", attrs);

  napi_value children;
  napi_create_array_with_length(env, node.children.size(), &children);
  for (size_t i = 0; i < node.children.size(); ++i) {
    napi_set_element(env, children, static_cast<uint32_t>(i), XmlNodeToNapi(env, node.children[i]));
  }
  napi_set_named_property(env, obj, "children", children);

  return obj;
}

static bool XmlRepairConfigFromNapi(napi_env env, napi_value val, llm_structured::XmlRepairConfig& cfg) {
  napi_valuetype vtype;
  napi_typeof(env, val, &vtype);
  if (vtype == napi_undefined || vtype == napi_null) return true;
  if (vtype != napi_object) {
    ThrowTypeError(env, "XmlRepairConfig must be an object");
    return false;
  }
  bool has = false;
  napi_has_named_property(env, val, "html_mode", &has);
  if (has) { napi_value v; napi_get_named_property(env, val, "html_mode", &v); napi_get_value_bool(env, v, &cfg.html_mode); }
  napi_has_named_property(env, val, "fix_unquoted_attributes", &has);
  if (has) { napi_value v; napi_get_named_property(env, val, "fix_unquoted_attributes", &v); napi_get_value_bool(env, v, &cfg.fix_unquoted_attributes); }
  napi_has_named_property(env, val, "auto_close_tags", &has);
  if (has) { napi_value v; napi_get_named_property(env, val, "auto_close_tags", &v); napi_get_value_bool(env, v, &cfg.auto_close_tags); }
  napi_has_named_property(env, val, "normalize_whitespace", &has);
  if (has) { napi_value v; napi_get_named_property(env, val, "normalize_whitespace", &v); napi_get_value_bool(env, v, &cfg.normalize_whitespace); }
  napi_has_named_property(env, val, "lowercase_names", &has);
  if (has) { napi_value v; napi_get_named_property(env, val, "lowercase_names", &v); napi_get_value_bool(env, v, &cfg.lowercase_names); }
  napi_has_named_property(env, val, "decode_entities", &has);
  if (has) { napi_value v; napi_get_named_property(env, val, "decode_entities", &v); napi_get_value_bool(env, v, &cfg.decode_entities); }
  return true;
}

static napi_value XmlRepairMetadataToNapi(napi_env env, const llm_structured::XmlRepairMetadata& meta) {
  napi_value obj;
  napi_create_object(env, &obj);
  napi_value v;
  napi_create_int32(env, meta.auto_closed_tags ? 1 : 0, &v);
  napi_set_named_property(env, obj, "auto_closed_tags", v);
  napi_create_int32(env, meta.fixed_unquoted_attributes ? 1 : 0, &v);
  napi_set_named_property(env, obj, "fixed_attributes", v);
  napi_create_int32(env, meta.decoded_entities ? 1 : 0, &v);
  napi_set_named_property(env, obj, "decoded_entities", v);
  napi_create_int32(env, meta.normalized_whitespace ? 1 : 0, &v);
  napi_set_named_property(env, obj, "normalized_whitespace", v);

  napi_create_int32(env, meta.unclosed_tag_count, &v);
  napi_set_named_property(env, obj, "unclosed_tag_count", v);
  return obj;
}

static llm_structured::XmlNode NapiToXmlNode(napi_env env, napi_value val) {
  llm_structured::XmlNode node;
  napi_value v;
  std::string str;
  
  napi_get_named_property(env, val, "type", &v);
  GetStringUtf8(env, v, str);
  if (str == "element") node.type = llm_structured::XmlNode::Type::Element;
  else if (str == "text") node.type = llm_structured::XmlNode::Type::Text;
  else if (str == "comment") node.type = llm_structured::XmlNode::Type::Comment;
  else if (str == "cdata") node.type = llm_structured::XmlNode::Type::CData;
  else if (str == "processing_instruction") node.type = llm_structured::XmlNode::Type::ProcessingInstruction;
  else if (str == "doctype") node.type = llm_structured::XmlNode::Type::Doctype;
  
  napi_get_named_property(env, val, "name", &v);
  GetStringUtf8(env, v, node.name);
  
  napi_get_named_property(env, val, "text", &v);
  GetStringUtf8(env, v, node.text);
  
  napi_get_named_property(env, val, "attributes", &v);
  napi_valuetype vtype;
  napi_typeof(env, v, &vtype);
  if (vtype == napi_object) {
    napi_value keys;
    napi_get_property_names(env, v, &keys);
    uint32_t len = 0;
    napi_get_array_length(env, keys, &len);
    for (uint32_t i = 0; i < len; ++i) {
      napi_value key;
      napi_get_element(env, keys, i, &key);
      std::string key_str, val_str;
      GetStringUtf8(env, key, key_str);
      napi_value attr_val;
      napi_get_property(env, v, key, &attr_val);
      GetStringUtf8(env, attr_val, val_str);
      node.attributes[key_str] = val_str;
    }
  }
  
  napi_get_named_property(env, val, "children", &v);
  bool is_array = false;
  napi_is_array(env, v, &is_array);
  if (is_array) {
    uint32_t len = 0;
    napi_get_array_length(env, v, &len);
    for (uint32_t i = 0; i < len; ++i) {
      napi_value child;
      napi_get_element(env, v, i, &child);
      node.children.push_back(NapiToXmlNode(env, child));
    }
  }
  
  return node;
}

// ---- XML / HTML functions ----

static napi_value ExtractXmlCandidate(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "extractXmlCandidate(text, rootTag?) expects 1-2 arguments");
    return nullptr;
  }
  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractXmlCandidate expects string");
    return nullptr;
  }
  (void)argc;
  return MakeString(env, llm_structured::extract_xml_candidate(text));
}

static napi_value ExtractXmlCandidates(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "extractXmlCandidates(text, rootTag?) expects 1-2 arguments");
    return nullptr;
  }
  std::string text;
  if (!GetStringUtf8(env, argv[0], text)) {
    ThrowTypeError(env, "extractXmlCandidates expects string");
    return nullptr;
  }
  (void)argc;
  auto results = llm_structured::extract_xml_candidates(text);
  napi_value arr;
  napi_create_array_with_length(env, results.size(), &arr);
  for (size_t i = 0; i < results.size(); ++i) {
    napi_set_element(env, arr, static_cast<uint32_t>(i), MakeString(env, results[i]));
  }
  return arr;
}

static napi_value LoadsXml(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "loadsXml(xmlString) expects 1 argument");
    return nullptr;
  }
  std::string xml_string;
  if (!GetStringUtf8(env, argv[0], xml_string)) {
    ThrowTypeError(env, "loadsXml expects string");
    return nullptr;
  }

  napi_value obj;
  napi_create_object(env, &obj);
  napi_value ok_val;

  try {
    auto root = llm_structured::loads_xml(xml_string);
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, ""));
    napi_set_named_property(env, obj, "root", XmlNodeToNapi(env, root));
  } catch (const std::exception& e) {
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, e.what()));
    napi_value null_val;
    napi_get_null(env, &null_val);
    napi_set_named_property(env, obj, "root", null_val);
  }

  return obj;
}

static napi_value LoadsXmlEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsXmlEx(xmlString, repair?) expects 1-2 arguments");
    return nullptr;
  }
  std::string xml_string;
  if (!GetStringUtf8(env, argv[0], xml_string)) {
    ThrowTypeError(env, "loadsXmlEx expects string");
    return nullptr;
  }
  llm_structured::XmlRepairConfig repair;
  if (argc >= 2) {
    if (!XmlRepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value ok_val;
  try {
    auto result = llm_structured::loads_xml_ex(xml_string, repair);
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, ""));
    napi_set_named_property(env, obj, "root", XmlNodeToNapi(env, result.root));
    napi_set_named_property(env, obj, "metadata", XmlRepairMetadataToNapi(env, result.metadata));
  } catch (const std::exception& e) {
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, e.what()));
    napi_value null_val;
    napi_get_null(env, &null_val);
    napi_set_named_property(env, obj, "root", null_val);
    napi_set_named_property(env, obj, "metadata", XmlRepairMetadataToNapi(env, llm_structured::XmlRepairMetadata{}));
  }

  return obj;
}

static napi_value LoadsHtml(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "loadsHtml(htmlString) expects 1 argument");
    return nullptr;
  }
  std::string html_string;
  if (!GetStringUtf8(env, argv[0], html_string)) {
    ThrowTypeError(env, "loadsHtml expects string");
    return nullptr;
  }
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value ok_val;
  try {
    auto root = llm_structured::loads_html(html_string);
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, ""));
    napi_set_named_property(env, obj, "root", XmlNodeToNapi(env, root));
  } catch (const std::exception& e) {
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, e.what()));
    napi_value null_val;
    napi_get_null(env, &null_val);
    napi_set_named_property(env, obj, "root", null_val);
  }

  return obj;
}

static napi_value LoadsHtmlEx(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "loadsHtmlEx(htmlString, repair?) expects 1-2 arguments");
    return nullptr;
  }
  std::string html_string;
  if (!GetStringUtf8(env, argv[0], html_string)) {
    ThrowTypeError(env, "loadsHtmlEx expects string");
    return nullptr;
  }
  llm_structured::XmlRepairConfig repair;
  if (argc >= 2) {
    if (!XmlRepairConfigFromNapi(env, argv[1], repair)) return nullptr;
  }
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value ok_val;
  try {
    auto result = llm_structured::loads_html_ex(html_string, repair);
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, ""));
    napi_set_named_property(env, obj, "root", XmlNodeToNapi(env, result.root));
    napi_set_named_property(env, obj, "metadata", XmlRepairMetadataToNapi(env, result.metadata));
  } catch (const std::exception& e) {
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, e.what()));
    napi_value null_val;
    napi_get_null(env, &null_val);
    napi_set_named_property(env, obj, "root", null_val);
    napi_set_named_property(env, obj, "metadata", XmlRepairMetadataToNapi(env, llm_structured::XmlRepairMetadata{}));
  }

  return obj;
}

static napi_value LoadsXmlAsJson(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "loadsXmlAsJson(xmlString) expects 1 argument");
    return nullptr;
  }
  std::string xml_string;
  if (!GetStringUtf8(env, argv[0], xml_string)) {
    ThrowTypeError(env, "loadsXmlAsJson expects string");
    return nullptr;
  }
  auto result = llm_structured::loads_xml_as_json(xml_string);
  return ToNapi(env, result);
}

static napi_value LoadsHtmlAsJson(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "loadsHtmlAsJson(htmlString) expects 1 argument");
    return nullptr;
  }
  std::string html_string;
  if (!GetStringUtf8(env, argv[0], html_string)) {
    ThrowTypeError(env, "loadsHtmlAsJson expects string");
    return nullptr;
  }
  auto result = llm_structured::loads_html_as_json(html_string);
  return ToNapi(env, result);
}

static napi_value DumpsXml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "dumpsXml(node, indent?) expects 1-2 arguments");
    return nullptr;
  }
  auto node = NapiToXmlNode(env, argv[0]);
  int indent = 2;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[1], &indent);
  }
  return MakeString(env, llm_structured::dumps_xml(node, indent));
}

static napi_value DumpsHtml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "dumpsHtml(node, indent?) expects 1-2 arguments");
    return nullptr;
  }
  auto node = NapiToXmlNode(env, argv[0]);
  int indent = 2;
  if (argc >= 2) {
    napi_get_value_int32(env, argv[1], &indent);
  }
  return MakeString(env, llm_structured::dumps_html(node, indent));
}

static napi_value QueryXml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "queryXml(node, selector) expects 2 arguments");
    return nullptr;
  }
  auto node = NapiToXmlNode(env, argv[0]);
  std::string selector;
  if (!GetStringUtf8(env, argv[1], selector)) {
    ThrowTypeError(env, "queryXml expects string selector");
    return nullptr;
  }
  auto results = llm_structured::query_xml(node, selector);
  napi_value arr;
  napi_create_array_with_length(env, results.size(), &arr);
  for (size_t i = 0; i < results.size(); ++i) {
    if (results[i]) {
      napi_set_element(env, arr, static_cast<uint32_t>(i), XmlNodeToNapi(env, *results[i]));
    } else {
      napi_value null_val;
      napi_get_null(env, &null_val);
      napi_set_element(env, arr, static_cast<uint32_t>(i), null_val);
    }
  }
  return arr;
}

static napi_value XmlTextContent(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "xmlTextContent(node) expects 1 argument");
    return nullptr;
  }
  auto node = NapiToXmlNode(env, argv[0]);
  return MakeString(env, llm_structured::xml_text_content(node));
}

static napi_value XmlGetAttribute(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "xmlGetAttribute(node, attrName) expects 2 arguments");
    return nullptr;
  }
  auto node = NapiToXmlNode(env, argv[0]);
  std::string attr_name;
  if (!GetStringUtf8(env, argv[1], attr_name)) {
    ThrowTypeError(env, "xmlGetAttribute expects string attrName");
    return nullptr;
  }
  const auto result = llm_structured::xml_get_attribute(node, attr_name);
  if (!result.empty()) return MakeString(env, result);
  napi_value null_val;
  napi_get_null(env, &null_val);
  return null_val;
}

static napi_value ValidateXml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "validateXml(node, schemaJson) expects 2 arguments");
    return nullptr;
  }
  auto node = NapiToXmlNode(env, argv[0]);
  std::string schema_text;
  if (!GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "validateXml expects string schemaJson");
    return nullptr;
  }
  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_value errors;
    napi_create_array_with_length(env, 0, &errors);

    llm_structured::validate_xml(node, schema, "$");

    napi_value ok_val;
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "errors", errors);
    return obj;
  } catch (const ValidationError& e) {
    napi_value obj;
    napi_create_object(env, &obj);
    napi_value ok_val;
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_value errors;
    napi_create_array_with_length(env, 1, &errors);
    napi_value err;
    napi_create_object(env, &err);
    napi_set_named_property(env, err, "path", MakeString(env, e.path));
    napi_set_named_property(env, err, "message", MakeString(env, e.what()));
    napi_set_element(env, errors, 0, err);
    napi_set_named_property(env, obj, "errors", errors);
    return obj;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "validation");
    return nullptr;
  }
}

static napi_value ParseAndValidateXml(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateXml(xmlString, schemaJson) expects 2 arguments");
    return nullptr;
  }
  std::string xml_string;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], xml_string) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateXml expects strings");
    return nullptr;
  }
  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_value errors;

    auto root = llm_structured::parse_and_validate_xml(xml_string, schema);

    napi_value ok_val;
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, ""));
    napi_set_named_property(env, obj, "root", XmlNodeToNapi(env, root));

    napi_create_array_with_length(env, 0, &errors);
    napi_set_named_property(env, obj, "validation_errors", errors);
    return obj;
  } catch (const ValidationError& e) {
    napi_value obj;
    napi_create_object(env, &obj);
    napi_value ok_val;
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, e.what()));
    napi_value null_val;
    napi_get_null(env, &null_val);
    napi_set_named_property(env, obj, "root", null_val);
    napi_value errors;
    napi_create_array_with_length(env, 1, &errors);
    napi_value err;
    napi_create_object(env, &err);
    napi_set_named_property(env, err, "path", MakeString(env, e.path));
    napi_set_named_property(env, err, "message", MakeString(env, e.what()));
    napi_set_element(env, errors, 0, err);
    napi_set_named_property(env, obj, "validation_errors", errors);
    return obj;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateXmlEx(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 2 || argc > 3) {
    ThrowTypeError(env, "parseAndValidateXmlEx(xmlString, schemaJson, repair?) expects 2-3 arguments");
    return nullptr;
  }
  std::string xml_string;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], xml_string) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateXmlEx expects strings");
    return nullptr;
  }
  llm_structured::XmlRepairConfig repair;
  if (argc >= 3) {
    if (!XmlRepairConfigFromNapi(env, argv[2], repair)) return nullptr;
  }
  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    napi_value obj;
    napi_create_object(env, &obj);
    napi_value errors;

    auto result = llm_structured::parse_and_validate_xml_ex(xml_string, schema, repair);

    napi_value ok_val;
    napi_get_boolean(env, true, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, ""));
    napi_set_named_property(env, obj, "root", XmlNodeToNapi(env, result.root));

    napi_create_array_with_length(env, 0, &errors);
    napi_set_named_property(env, obj, "validation_errors", errors);
    napi_set_named_property(env, obj, "metadata", XmlRepairMetadataToNapi(env, result.metadata));
    return obj;
  } catch (const ValidationError& e) {
    napi_value obj;
    napi_create_object(env, &obj);
    napi_value ok_val;
    napi_get_boolean(env, false, &ok_val);
    napi_set_named_property(env, obj, "ok", ok_val);
    napi_set_named_property(env, obj, "error", MakeString(env, e.what()));
    napi_value null_val;
    napi_get_null(env, &null_val);
    napi_set_named_property(env, obj, "root", null_val);
    napi_value errors;
    napi_create_array_with_length(env, 1, &errors);
    napi_value err;
    napi_create_object(env, &err);
    napi_set_named_property(env, err, "path", MakeString(env, e.path));
    napi_set_named_property(env, err, "message", MakeString(env, e.what()));
    napi_set_element(env, errors, 0, err);
    napi_set_named_property(env, obj, "validation_errors", errors);
    napi_set_named_property(env, obj, "metadata", XmlRepairMetadataToNapi(env, llm_structured::XmlRepairMetadata{}));
    return obj;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateSql(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateSql(sqlText, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateSql(sqlText, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto parsed = llm_structured::parse_and_validate_sql(text, schema);

    return ToNapiSqlParsed(env, parsed);
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static void FinalizeJsonStreamParser(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamParser*>(data);
}

static void FinalizeJsonStreamCollector(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamCollector*>(data);
}

static void FinalizeJsonStreamBatchCollector(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamBatchCollector*>(data);
}

static void FinalizeJsonStreamValidatedBatchCollector(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::JsonStreamValidatedBatchCollector*>(data);
}

static void FinalizeSqlStreamParser(napi_env /*env*/, void* data, void* /*hint*/) {
  delete static_cast<llm_structured::SqlStreamParser*>(data);
}

static napi_value CreateJsonStreamParser(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamParser(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamParser(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamParser(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
      }
    }

    auto* p = (max_buffer_bytes > 0)
                  ? new llm_structured::JsonStreamParser(std::move(schema), max_buffer_bytes)
                  : new llm_structured::JsonStreamParser(std::move(schema));
    napi_value ext;
    napi_create_external(env, p, FinalizeJsonStreamParser, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamParserFinish(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserFinish(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserFinish(parser) expects a parser external");
    return nullptr;
  }

  p->finish();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamParserLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserLocation(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserLocation(parser) expects a parser external");
    return nullptr;
  }

  return StreamLocationToNapi(env, p->location());
}

static napi_value CreateJsonStreamCollector(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamCollector(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamCollector(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    size_t max_items = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamCollector(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
        if (!GetOptionalSizeTProperty(env, argv[1], "maxItems", max_items)) return nullptr;
      }
    }

    auto* c = (max_buffer_bytes > 0 || max_items > 0)
                  ? new llm_structured::JsonStreamCollector(std::move(schema), max_buffer_bytes, max_items)
                  : new llm_structured::JsonStreamCollector(std::move(schema));
    napi_value ext;
    napi_create_external(env, c, FinalizeJsonStreamCollector, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamCollectorLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorLocation(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorLocation(collector) expects a collector external");
    return nullptr;
  }
  return StreamLocationToNapi(env, c->location());
}

static napi_value CreateJsonStreamBatchCollector(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamBatchCollector(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamBatchCollector(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    size_t max_items = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamBatchCollector(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
        if (!GetOptionalSizeTProperty(env, argv[1], "maxItems", max_items)) return nullptr;
      }
    }

    auto* c = (max_buffer_bytes > 0 || max_items > 0)
                  ? new llm_structured::JsonStreamBatchCollector(std::move(schema), max_buffer_bytes, max_items)
                  : new llm_structured::JsonStreamBatchCollector(std::move(schema));
    napi_value ext;
    napi_create_external(env, c, FinalizeJsonStreamBatchCollector, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamBatchCollectorLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorLocation(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorLocation(collector) expects a collector external");
    return nullptr;
  }
  return StreamLocationToNapi(env, c->location());
}

static napi_value CreateJsonStreamValidatedBatchCollector(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createJsonStreamValidatedBatchCollector(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createJsonStreamValidatedBatchCollector(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    size_t max_items = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createJsonStreamValidatedBatchCollector(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
        if (!GetOptionalSizeTProperty(env, argv[1], "maxItems", max_items)) return nullptr;
      }
    }

    auto* c = (max_buffer_bytes > 0 || max_items > 0)
                  ? new llm_structured::JsonStreamValidatedBatchCollector(std::move(schema), max_buffer_bytes, max_items)
                  : new llm_structured::JsonStreamValidatedBatchCollector(std::move(schema));
    napi_value ext;
    napi_create_external(env, c, FinalizeJsonStreamValidatedBatchCollector, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value JsonStreamValidatedBatchCollectorReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorReset(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorReset(collector) expects a collector external");
    return nullptr;
  }
  c->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamValidatedBatchCollectorAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorAppend(collector, chunk) expects 2 arguments");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorAppend(collector, chunk) expects a collector external");
    return nullptr;
  }
  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorAppend(collector, chunk) expects chunk as string");
    return nullptr;
  }
  c->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamValidatedBatchCollectorClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorClose(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorClose(collector) expects a collector external");
    return nullptr;
  }
  c->close();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamValidatedBatchCollectorPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorPoll(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorPoll(collector) expects a collector external");
    return nullptr;
  }

  auto out = c->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_value arr = ToNapiArray(env, *out.value);
    napi_set_named_property(env, obj, "value", arr);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value JsonStreamValidatedBatchCollectorLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorLocation(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamValidatedBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamValidatedBatchCollectorLocation(collector) expects a collector external");
    return nullptr;
  }
  return StreamLocationToNapi(env, c->location());
}

static napi_value JsonStreamParserReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserReset(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserReset(parser) expects a parser external");
    return nullptr;
  }
  p->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamParserAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamParserAppend(parser, chunk) expects 2 arguments");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserAppend(parser, chunk) expects a parser external");
    return nullptr;
  }

  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamParserAppend(parser, chunk) expects chunk as string");
    return nullptr;
  }

  p->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamParserPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamParserPoll(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::JsonStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "jsonStreamParserPoll(parser) expects a parser external");
    return nullptr;
  }

  auto out = p->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_set_named_property(env, obj, "value", ToNapi(env, *out.value));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value JsonStreamCollectorReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorReset(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorReset(collector) expects a collector external");
    return nullptr;
  }
  c->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamCollectorAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamCollectorAppend(collector, chunk) expects 2 arguments");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorAppend(collector, chunk) expects a collector external");
    return nullptr;
  }
  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamCollectorAppend(collector, chunk) expects chunk as string");
    return nullptr;
  }
  c->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamCollectorClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorClose(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorClose(collector) expects a collector external");
    return nullptr;
  }
  c->close();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamCollectorPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamCollectorPoll(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamCollectorPoll(collector) expects a collector external");
    return nullptr;
  }

  auto out = c->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_value arr = ToNapiArray(env, *out.value);
    napi_set_named_property(env, obj, "value", arr);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value JsonStreamBatchCollectorReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorReset(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorReset(collector) expects a collector external");
    return nullptr;
  }
  c->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamBatchCollectorAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "jsonStreamBatchCollectorAppend(collector, chunk) expects 2 arguments");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorAppend(collector, chunk) expects a collector external");
    return nullptr;
  }
  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorAppend(collector, chunk) expects chunk as string");
    return nullptr;
  }
  c->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamBatchCollectorClose(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorClose(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorClose(collector) expects a collector external");
    return nullptr;
  }
  c->close();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value JsonStreamBatchCollectorPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "jsonStreamBatchCollectorPoll(collector) expects 1 argument");
    return nullptr;
  }
  llm_structured::JsonStreamBatchCollector* c = nullptr;
  if (!GetExternalPtr(env, argv[0], c)) {
    ThrowTypeError(env, "jsonStreamBatchCollectorPoll(collector) expects a collector external");
    return nullptr;
  }

  auto out = c->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_value arr = ToNapiArray(env, *out.value);
    napi_set_named_property(env, obj, "value", arr);
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value CreateSqlStreamParser(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc < 1 || argc > 2) {
    ThrowTypeError(env, "createSqlStreamParser(schemaJson, limits?) expects 1-2 arguments");
    return nullptr;
  }

  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], schema_text)) {
    ThrowTypeError(env, "createSqlStreamParser(schemaJson) expects a string");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);

    size_t max_buffer_bytes = 0;
    if (argc == 2) {
      napi_valuetype t;
      if (napi_typeof(env, argv[1], &t) != napi_ok) return nullptr;
      if (t != napi_undefined && t != napi_null) {
        if (t != napi_object) {
          ThrowTypeError(env, "createSqlStreamParser(..., limits) expects limits as object");
          return nullptr;
        }
        if (!GetOptionalSizeTProperty(env, argv[1], "maxBufferBytes", max_buffer_bytes)) return nullptr;
      }
    }

    auto* p = (max_buffer_bytes > 0)
                  ? new llm_structured::SqlStreamParser(std::move(schema), max_buffer_bytes)
                  : new llm_structured::SqlStreamParser(std::move(schema));
    napi_value ext;
    napi_create_external(env, p, FinalizeSqlStreamParser, nullptr, &ext);
    return ext;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value SqlStreamParserFinish(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserFinish(parser) expects 1 argument");
    return nullptr;
  }
  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserFinish(parser) expects a parser external");
    return nullptr;
  }
  p->finish();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value SqlStreamParserLocation(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserLocation(parser) expects 1 argument");
    return nullptr;
  }
  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserLocation(parser) expects a parser external");
    return nullptr;
  }
  return StreamLocationToNapi(env, p->location());
}

static napi_value SqlStreamParserReset(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserReset(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserReset(parser) expects a parser external");
    return nullptr;
  }
  p->reset();
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value SqlStreamParserAppend(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "sqlStreamParserAppend(parser, chunk) expects 2 arguments");
    return nullptr;
  }

  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserAppend(parser, chunk) expects a parser external");
    return nullptr;
  }

  std::string chunk;
  if (!GetStringUtf8(env, argv[1], chunk)) {
    ThrowTypeError(env, "sqlStreamParserAppend(parser, chunk) expects chunk as string");
    return nullptr;
  }

  p->append(chunk);
  napi_value undef;
  napi_get_undefined(env, &undef);
  return undef;
}

static napi_value SqlStreamParserPoll(napi_env env, napi_callback_info info) {
  size_t argc = 1;
  napi_value argv[1];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 1) {
    ThrowTypeError(env, "sqlStreamParserPoll(parser) expects 1 argument");
    return nullptr;
  }

  llm_structured::SqlStreamParser* p = nullptr;
  if (!GetExternalPtr(env, argv[0], p)) {
    ThrowTypeError(env, "sqlStreamParserPoll(parser) expects a parser external");
    return nullptr;
  }

  auto out = p->poll();
  napi_value obj;
  napi_create_object(env, &obj);

  napi_value b;
  napi_get_boolean(env, out.done, &b);
  napi_set_named_property(env, obj, "done", b);
  napi_get_boolean(env, out.ok, &b);
  napi_set_named_property(env, obj, "ok", b);

  if (out.value) {
    napi_set_named_property(env, obj, "value", ToNapiSqlParsed(env, *out.value));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "value", n);
  }

  if (out.error) {
    napi_set_named_property(env, obj, "error", MakeValidationErrorObject(env, *out.error));
  } else {
    napi_value n;
    napi_get_null(env, &n);
    napi_set_named_property(env, obj, "error", n);
  }

  return obj;
}

static napi_value ParseAndValidateMarkdown(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateMarkdown(markdownText, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateMarkdown(markdownText, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto parsed = llm_structured::parse_and_validate_markdown(text, schema);

    napi_value obj;
    napi_create_object(env, &obj);

    napi_value n;
    napi_create_int32(env, static_cast<int32_t>(parsed.headings.size()), &n);
    napi_set_named_property(env, obj, "headingCount", n);
    napi_create_int32(env, static_cast<int32_t>(parsed.codeBlocks.size()), &n);
    napi_set_named_property(env, obj, "codeBlockCount", n);
    napi_create_int32(env, static_cast<int32_t>(parsed.tables.size()), &n);
    napi_set_named_property(env, obj, "tableCount", n);
    napi_create_int32(env, static_cast<int32_t>(parsed.taskLineNumbers.size()), &n);
    napi_set_named_property(env, obj, "taskCount", n);

    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

static napi_value ParseAndValidateKv(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  if (argc != 2) {
    ThrowTypeError(env, "parseAndValidateKv(text, schemaJson) expects 2 arguments");
    return nullptr;
  }

  std::string text;
  std::string schema_text;
  if (!GetStringUtf8(env, argv[0], text) || !GetStringUtf8(env, argv[1], schema_text)) {
    ThrowTypeError(env, "parseAndValidateKv(text, schemaJson) expects strings");
    return nullptr;
  }

  try {
    Json schema = llm_structured::loads_jsonish(schema_text);
    auto kv = llm_structured::parse_and_validate_kv(text, schema);
    napi_value obj;
    napi_create_object(env, &obj);
    for (const auto& it : kv) {
      napi_set_named_property(env, obj, it.first.c_str(), MakeString(env, it.second));
    }
    return obj;
  } catch (const ValidationError& e) {
    ThrowValidationError(env, e);
    return nullptr;
  } catch (const std::exception& e) {
    ThrowErrorWithKind(env, e.what(), "parse");
    return nullptr;
  }
}

// ---- Schema Inference helpers ----

static llm_structured::SchemaInferenceConfig SchemaInferenceConfigFromNapi(napi_env env, napi_value cfg_val) {
  llm_structured::SchemaInferenceConfig cfg;
  if (cfg_val == nullptr) return cfg;
  
  napi_valuetype vt;
  if (napi_typeof(env, cfg_val, &vt) != napi_ok || vt != napi_object) return cfg;
  
  auto get_bool = [&](const char* key, bool& out) {
    napi_value v;
    if (napi_get_named_property(env, cfg_val, key, &v) == napi_ok) {
      napi_valuetype t;
      if (napi_typeof(env, v, &t) == napi_ok && t == napi_boolean) {
        napi_get_value_bool(env, v, &out);
      }
    }
  };
  
  auto get_int = [&](const char* key, int& out) {
    napi_value v;
    if (napi_get_named_property(env, cfg_val, key, &v) == napi_ok) {
      napi_valuetype t;
      if (napi_typeof(env, v, &t) == napi_ok && t == napi_number) {
        int32_t n;
        napi_get_value_int32(env, v, &n);
        out = n;
      }
    }
  };
  
  get_bool("includeExamples", cfg.include_examples);
  get_int("maxExamples", cfg.max_examples);
  get_bool("includeDefault", cfg.include_default);
  get_bool("inferFormats", cfg.infer_formats);
  get_bool("inferPatterns", cfg.infer_patterns);
  get_bool("inferNumericRanges", cfg.infer_numeric_ranges);
  get_bool("inferStringLengths", cfg.infer_string_lengths);
  get_bool("inferArrayLengths", cfg.infer_array_lengths);
  get_bool("requiredByDefault", cfg.required_by_default);
  get_bool("strictAdditionalProperties", cfg.strict_additional_properties);
  get_bool("preferInteger", cfg.prefer_integer);
  get_bool("allowAnyOf", cfg.allow_any_of);
  get_bool("includeDescriptions", cfg.include_descriptions);
  get_bool("detectEnums", cfg.detect_enums);
  get_int("maxEnumValues", cfg.max_enum_values);
  
  return cfg;
}

static napi_value InferSchema(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  
  if (argc < 1) {
    ThrowTypeError(env, "inferSchema(value, config?) expects at least 1 argument");
    return nullptr;
  }
  
  llm_structured::Json value;
  if (!FromNapi(env, argv[0], value)) {
    ThrowTypeError(env, "inferSchema(value, config?) received an unsupported JS value");
    return nullptr;
  }
  llm_structured::SchemaInferenceConfig cfg;
  if (argc >= 2) {
    cfg = SchemaInferenceConfigFromNapi(env, argv[1]);
  }
  
  try {
    llm_structured::Json schema = llm_structured::infer_schema(value, cfg);
    return ToNapi(env, schema);
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value InferSchemaFromValues(napi_env env, napi_callback_info info) {
  size_t argc = 2;
  napi_value argv[2];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  
  if (argc < 1) {
    ThrowTypeError(env, "inferSchemaFromValues(values, config?) expects at least 1 argument");
    return nullptr;
  }
  
  // Check if first arg is an array
  bool is_array = false;
  napi_is_array(env, argv[0], &is_array);
  if (!is_array) {
    ThrowTypeError(env, "inferSchemaFromValues(values, config?) expects values to be an array");
    return nullptr;
  }
  
  uint32_t length;
  napi_get_array_length(env, argv[0], &length);
  
  llm_structured::JsonArray values;
  for (uint32_t i = 0; i < length; ++i) {
    napi_value item;
    napi_get_element(env, argv[0], i, &item);
    llm_structured::Json v;
    if (!FromNapi(env, item, v)) {
      ThrowTypeError(env, "inferSchemaFromValues(values, config?) received an unsupported JS value");
      return nullptr;
    }
    values.push_back(v);
  }
  
  llm_structured::SchemaInferenceConfig cfg;
  if (argc >= 2) {
    cfg = SchemaInferenceConfigFromNapi(env, argv[1]);
  }
  
  try {
    llm_structured::Json schema = llm_structured::infer_schema_from_values(values, cfg);
    return ToNapi(env, schema);
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value MergeSchemas(napi_env env, napi_callback_info info) {
  size_t argc = 3;
  napi_value argv[3];
  napi_value this_arg;
  void* data;
  if (napi_get_cb_info(env, info, &argc, argv, &this_arg, &data) != napi_ok) return nullptr;
  
  if (argc < 2) {
    ThrowTypeError(env, "mergeSchemas(schema1, schema2, config?) expects at least 2 arguments");
    return nullptr;
  }
  
  llm_structured::Json schema1;
  llm_structured::Json schema2;
  if (!FromNapi(env, argv[0], schema1) || !FromNapi(env, argv[1], schema2)) {
    ThrowTypeError(env, "mergeSchemas(schema1, schema2, config?) received an unsupported JS value");
    return nullptr;
  }
  
  llm_structured::SchemaInferenceConfig cfg;
  if (argc >= 3) {
    cfg = SchemaInferenceConfigFromNapi(env, argv[2]);
  }
  
  try {
    llm_structured::Json merged = llm_structured::merge_schemas(schema1, schema2, cfg);
    return ToNapi(env, merged);
  } catch (const std::exception& e) {
    ThrowError(env, e.what());
    return nullptr;
  }
}

static napi_value Init(napi_env env, napi_value exports) {
  napi_property_descriptor props[] = {
      {"parseAndValidateJson", nullptr, ParseAndValidateJson, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonWithDefaults", nullptr, ParseAndValidateJsonWithDefaults, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"loadsJsonishEx", nullptr, LoadsJsonishEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"extractJsonCandidates", nullptr, ExtractJsonCandidates, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"loadsJsonishAllEx", nullptr, LoadsJsonishAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonEx", nullptr, ParseAndValidateJsonEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonAllEx", nullptr, ParseAndValidateJsonAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndValidateJsonWithDefaultsEx", nullptr, ParseAndValidateJsonWithDefaultsEx, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"validateAllJson", nullptr, ValidateAllJson, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"validateAllJsonValue", nullptr, ValidateAllJsonValue, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"validateWithRepair", nullptr, ValidateWithRepair, nullptr, nullptr, nullptr, napi_default, nullptr},
  {"parseAndRepair", nullptr, ParseAndRepair, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"extractYamlCandidate", nullptr, ExtractYamlCandidate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"extractYamlCandidates", nullptr, ExtractYamlCandidates, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsYamlishEx", nullptr, LoadsYamlishEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsYamlishAllEx", nullptr, LoadsYamlishAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dumpsYaml", nullptr, DumpsYaml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateYaml", nullptr, ParseAndValidateYaml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateYamlEx", nullptr, ParseAndValidateYamlEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateYamlAllEx", nullptr, ParseAndValidateYamlAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"extractTomlCandidate", nullptr, ExtractTomlCandidate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"extractTomlCandidates", nullptr, ExtractTomlCandidates, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsTomlishEx", nullptr, LoadsTomlishEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsTomlishAllEx", nullptr, LoadsTomlishAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dumpsToml", nullptr, DumpsToml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateToml", nullptr, ParseAndValidateToml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateTomlEx", nullptr, ParseAndValidateTomlEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateTomlAllEx", nullptr, ParseAndValidateTomlAllEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"extractXmlCandidate", nullptr, ExtractXmlCandidate, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"extractXmlCandidates", nullptr, ExtractXmlCandidates, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsXml", nullptr, LoadsXml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsXmlEx", nullptr, LoadsXmlEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsHtml", nullptr, LoadsHtml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsHtmlEx", nullptr, LoadsHtmlEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsXmlAsJson", nullptr, LoadsXmlAsJson, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"loadsHtmlAsJson", nullptr, LoadsHtmlAsJson, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dumpsXml", nullptr, DumpsXml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"dumpsHtml", nullptr, DumpsHtml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"queryXml", nullptr, QueryXml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"xmlTextContent", nullptr, XmlTextContent, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"xmlGetAttribute", nullptr, XmlGetAttribute, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"validateXml", nullptr, ValidateXml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateXml", nullptr, ParseAndValidateXml, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateXmlEx", nullptr, ParseAndValidateXmlEx, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateSql", nullptr, ParseAndValidateSql, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateMarkdown", nullptr, ParseAndValidateMarkdown, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"parseAndValidateKv", nullptr, ParseAndValidateKv, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"createJsonStreamParser", nullptr, CreateJsonStreamParser, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserReset", nullptr, JsonStreamParserReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserFinish", nullptr, JsonStreamParserFinish, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserAppend", nullptr, JsonStreamParserAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserPoll", nullptr, JsonStreamParserPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"jsonStreamParserLocation", nullptr, JsonStreamParserLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createJsonStreamCollector", nullptr, CreateJsonStreamCollector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorReset", nullptr, JsonStreamCollectorReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorAppend", nullptr, JsonStreamCollectorAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorClose", nullptr, JsonStreamCollectorClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorPoll", nullptr, JsonStreamCollectorPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamCollectorLocation", nullptr, JsonStreamCollectorLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createJsonStreamBatchCollector", nullptr, CreateJsonStreamBatchCollector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorReset", nullptr, JsonStreamBatchCollectorReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorAppend", nullptr, JsonStreamBatchCollectorAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorClose", nullptr, JsonStreamBatchCollectorClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorPoll", nullptr, JsonStreamBatchCollectorPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamBatchCollectorLocation", nullptr, JsonStreamBatchCollectorLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"createJsonStreamValidatedBatchCollector", nullptr, CreateJsonStreamValidatedBatchCollector, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorReset", nullptr, JsonStreamValidatedBatchCollectorReset, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorAppend", nullptr, JsonStreamValidatedBatchCollectorAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorClose", nullptr, JsonStreamValidatedBatchCollectorClose, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorPoll", nullptr, JsonStreamValidatedBatchCollectorPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
        {"jsonStreamValidatedBatchCollectorLocation", nullptr, JsonStreamValidatedBatchCollectorLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"createSqlStreamParser", nullptr, CreateSqlStreamParser, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserReset", nullptr, SqlStreamParserReset, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserFinish", nullptr, SqlStreamParserFinish, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserAppend", nullptr, SqlStreamParserAppend, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserPoll", nullptr, SqlStreamParserPoll, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"sqlStreamParserLocation", nullptr, SqlStreamParserLocation, nullptr, nullptr, nullptr, napi_default, nullptr},
      // Schema Inference
      {"inferSchema", nullptr, InferSchema, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"inferSchemaFromValues", nullptr, InferSchemaFromValues, nullptr, nullptr, nullptr, napi_default, nullptr},
      {"mergeSchemas", nullptr, MergeSchemas, nullptr, nullptr, nullptr, napi_default, nullptr},
  };

  napi_define_properties(env, exports, sizeof(props) / sizeof(props[0]), props);
  return exports;
}

NAPI_MODULE(NODE_GYP_MODULE_NAME, Init)
