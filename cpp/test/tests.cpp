#include "llm_structured.hpp"

#include <cassert>
#include <iostream>
#include <string>

using namespace llm_structured;

static void test_json_extract_and_validate() {
  std::string text = "blah\n```json\n{\"name\":\"Ada\",\"age\":12,}\n```\n";

  JsonObject schema_props;
  schema_props["name"] = Json(JsonObject{{"type", "string"}, {"minLength", 1.0}});
  schema_props["age"] = Json(JsonObject{{"type", "integer"}, {"minimum", 0.0}});

  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("name"), Json("age")}},
      {"additionalProperties", Json(false)},
      {"properties", Json(schema_props)},
  });

  Json v = parse_and_validate(text, schema);
  assert(v.is_object());
  assert(v.as_object().at("name").as_string() == "Ada");
}

static void test_json_missing_required_path() {
  std::string text = "```json\n{\"name\":\"Ada\"}\n```\n";

  JsonObject schema_props;
  schema_props["name"] = Json(JsonObject{{"type", "string"}});
  schema_props["age"] = Json(JsonObject{{"type", "integer"}});

  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("name"), Json("age")}},
      {"properties", Json(schema_props)},
  });

  try {
    (void)parse_and_validate(text, schema);
    assert(false && "expected ValidationError");
  } catch (const ValidationError& e) {
    assert(e.path == "$.age");
  }
}

static void test_json_duplicate_key_policy() {
  // Default behavior is FirstWins.
  {
    auto r = loads_jsonish_ex("{\"a\":1,\"a\":2}", RepairConfig{});
    assert(r.value.is_object());
    assert(r.value.as_object().at("a").as_number() == 1);
    assert(r.metadata.duplicateKeyCount == 1);
    assert(r.metadata.duplicateKeyPolicy == RepairConfig::DuplicateKeyPolicy::FirstWins);
  }

  // LastWins overwrites.
  {
    RepairConfig cfg;
    cfg.duplicate_key_policy = RepairConfig::DuplicateKeyPolicy::LastWins;
    auto r = loads_jsonish_ex("{\"a\":1,\"a\":2}", cfg);
    assert(r.value.is_object());
    assert(r.value.as_object().at("a").as_number() == 2);
    assert(r.metadata.duplicateKeyCount == 1);
    assert(r.metadata.duplicateKeyPolicy == RepairConfig::DuplicateKeyPolicy::LastWins);
  }

  // Error rejects duplicates.
  {
    RepairConfig cfg;
    cfg.duplicate_key_policy = RepairConfig::DuplicateKeyPolicy::Error;
    try {
      (void)loads_jsonish_ex("{\"a\":1,\"a\":2}", cfg);
      assert(false && "expected ValidationError");
    } catch (const ValidationError& e) {
      assert(e.kind == "parse");
      assert(e.path == "$.a");
    }
  }
}

static void test_markdown_validate() {
  std::string md = "# Title\n\n## Tasks\n- [x] done\n";
  Json schema = Json(JsonObject{
      {"requiredHeadings", JsonArray{Json("Title")}},
      {"requireTaskList", Json(true)},
  });

  auto p = parse_and_validate_markdown(md, schema);
  assert(!p.taskLineNumbers.empty());
}

static void test_markdown_missing_heading_path() {
  std::string md = "# Title\n";
  Json schema = Json(JsonObject{
      {"requiredHeadings", JsonArray{Json("Intro")}},
  });

  try {
    (void)parse_and_validate_markdown(md, schema);
    assert(false && "expected ValidationError");
  } catch (const ValidationError& e) {
    assert(e.path == "$.headings[Intro]");
  }
}

static void test_markdown_line_too_long_path() {
  std::string md = "1234\n";
  Json schema = Json(JsonObject{
      {"maxLineLength", 3.0},
  });

  try {
    (void)parse_and_validate_markdown(md, schema);
    assert(false && "expected ValidationError");
  } catch (const ValidationError& e) {
    assert(e.path == "$.lines[1]");
  }
}

static void test_kv_validate() {
  std::string env = "A=1\nB=hello\n";
  Json schema = Json(JsonObject{
      {"required", JsonArray{Json("A")}},
      {"allowExtra", Json(true)},
      {"patterns", Json(JsonObject{{"A", Json("^\\d+$")}})},
  });
  auto kv = parse_and_validate_kv(env, schema);
  assert(kv.at("A") == "1");
}

static void test_kv_missing_required_path() {
  std::string env = "B=hello\n";
  Json schema = Json(JsonObject{
      {"required", JsonArray{Json("A")}},
  });

  try {
    (void)parse_and_validate_kv(env, schema);
    assert(false && "expected ValidationError");
  } catch (const ValidationError& e) {
    assert(e.path == "$.A");
  }
}

static void test_sql_validate() {
  std::string sql = "SELECT id FROM users WHERE id = 1 ORDER BY id DESC LIMIT 1";
  Json schema = Json(JsonObject{
      {"allowedStatements", JsonArray{Json("select")}},
      {"requireWhere", Json(true)},
      {"requireLimit", Json(true)},
      {"maxLimit", 10.0},
      {"forbidUnion", Json(true)},
      {"requireOrderBy", Json(true)},
      {"forbidSelectStar", Json(true)},
      {"allowedTables", JsonArray{Json("users")}},
  });

  auto p = parse_and_validate_sql(sql, schema);
  assert(p.statementType == "select");
}

static void test_sql_missing_limit_path() {
  std::string sql = "SELECT id FROM users WHERE id = 1 ORDER BY id DESC";
  Json schema = Json(JsonObject{
      {"allowedStatements", JsonArray{Json("select")}},
      {"requireLimit", Json(true)},
  });
  try {
    (void)parse_and_validate_sql(sql, schema);
    assert(false && "expected ValidationError");
  } catch (const ValidationError& e) {
    assert(e.path == "$.limit");
  }
}

static void test_json_stream_parser_success() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamParser p(schema);
  p.append("blah\n```json\n{");
  auto o1 = p.poll();
  assert(!o1.done);
  p.append("\"age\": 1}");
  auto o2 = p.poll();
  assert(!o2.done);
  p.append("\n```\n");
  auto o3 = p.poll();
  assert(o3.done);
  assert(o3.ok);
  assert(o3.value.has_value());
  assert(o3.value->is_object());
  assert(o3.value->as_object().at("age").as_number() == 1);
}

static void test_json_stream_parser_error() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamParser p(schema);
  p.append("```json\n{\"name\":\"Ada\"}\n```");
  auto o = p.poll();
  assert(o.done);
  assert(!o.ok);
  assert(o.error.has_value());
  assert(o.error->path == "$.age");
}

static void test_json_stream_parser_max_buffer_bytes() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamParser p(schema, 8);
  p.append("0123456789");
  auto out = p.poll();
  assert(out.done);
  assert(!out.ok);
  assert(out.error.has_value());
  assert(out.error->path == "$.stream.maxBufferBytes");
  // Message should include current/max (best-effort regression).
  assert(std::string(out.error->what()).find("max=8") != std::string::npos);
}

static void test_sql_stream_parser_success() {
  Json schema = Json(JsonObject{
      {"allowedStatements", JsonArray{Json("select")}},
      {"requireLimit", Json(true)},
  });

  SqlStreamParser p(schema);
  p.append("SELECT id FROM users ");
  auto o1 = p.poll();
  assert(!o1.done);
  p.append("WHERE id = 1 ");
  auto o2 = p.poll();
  assert(!o2.done);
  p.append("LIMIT 1;");
  auto o3 = p.poll();
  assert(o3.done);
  assert(o3.ok);
  assert(o3.value.has_value());
  assert(o3.value->hasLimit);
  assert(o3.value->limit.has_value());
  assert(*o3.value->limit == 1);
}

static void test_sql_stream_parser_error() {
  Json schema = Json(JsonObject{
      {"allowedStatements", JsonArray{Json("select")}},
      {"requireLimit", Json(true)},
  });

  SqlStreamParser p(schema);
  p.append("SELECT id FROM users WHERE id = 1;");
  auto o = p.poll();
  assert(o.done);
  assert(!o.ok);
  assert(o.error.has_value());
  assert(o.error->path == "$.limit");
}

static void test_sql_stream_parser_max_buffer_bytes() {
  Json schema = Json(JsonObject{
      {"allowedStatements", JsonArray{Json("select")}},
      {"requireLimit", Json(true)},
  });

  SqlStreamParser p(schema, 8);
  p.append("0123456789");
  auto out = p.poll();
  assert(out.done);
  assert(!out.ok);
  assert(out.error.has_value());
  assert(out.error->path == "$.stream.maxBufferBytes");
}

static void test_json_stream_collector_all_success() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamCollector c(schema);
  c.append("{\"age\": 1}\n");
  auto o1 = c.poll();
  assert(!o1.done);

  c.append("{\"age\": 2}\n");
  auto o2 = c.poll();
  assert(!o2.done);

  c.close();
  auto o3 = c.poll();
  assert(o3.done);
  assert(o3.ok);
  assert(o3.value.has_value());
  assert(o3.value->size() == 2);
  assert((*o3.value)[0].as_object().at("age").as_number() == 1);
  assert((*o3.value)[1].as_object().at("age").as_number() == 2);
}

static void test_json_stream_collector_all_error() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamCollector c(schema);
  c.append("{\"age\": 1}\n{\"name\": \"Ada\"}\n");
  auto out = c.poll();
  assert(out.done);
  assert(!out.ok);
  assert(out.error.has_value());
  assert(out.error->path == "$.age");
}

static void test_json_stream_collector_max_items() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamCollector c(schema, 0, 1);
  c.append("{\"age\": 1}\n{\"age\": 2}\n");
  c.close();
  auto out = c.poll();
  assert(out.done);
  assert(!out.ok);
  assert(out.error.has_value());
  assert(out.error->path == "$.stream.maxItems");
  assert(std::string(out.error->what()).find("max=1") != std::string::npos);
}

static bool has_error_path(const std::vector<ValidationError>& errs, const std::string& path) {
  for (const auto& e : errs) {
    if (e.path == path) return true;
  }
  return false;
}

static void test_validate_all_collects_multiple_errors() {
  Json value = loads_jsonish("{\"age\": -1, \"extra\": 1}");
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("name")}},
      {"additionalProperties", Json(false)},
      {"properties",
       Json(JsonObject{
           {"name", Json(JsonObject{{"type", "string"}, {"minLength", 2.0}})},
           {"age", Json(JsonObject{{"type", "integer"}, {"minimum", 0.0}})},
       })},
  });

  auto errs = validate_all(value, schema);
  assert(errs.size() >= 2);
  assert(has_error_path(errs, "$.name"));
  // Either age minimum or extra forbidden or both should be present.
  assert(has_error_path(errs, "$.age") || has_error_path(errs, "$.extra"));
}

static void test_parse_and_validate_with_defaults_fills() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("name"), Json("age")}},
      {"properties",
       Json(JsonObject{
           {"name", Json(JsonObject{{"type", "string"}})},
           {"age", Json(JsonObject{{"type", "integer"}, {"default", 18.0}})},
       })},
  });
  Json v = parse_and_validate_with_defaults("{\"name\":\"Ada\"}", schema);
  assert(v.is_object());
  assert(v.as_object().at("age").as_number() == 18);
}

static void test_schema_combinators_anyof_oneof() {
  Json any_schema = Json(JsonObject{
      {"anyOf",
       JsonArray{
           Json(JsonObject{{"type", "string"}}),
           Json(JsonObject{{"type", "number"}}),
       }},
  });
  auto errs_any = validate_all(Json(true), any_schema);
  assert(!errs_any.empty());

  Json one_schema = Json(JsonObject{
      {"oneOf",
       JsonArray{
           Json(JsonObject{{"type", "number"}}),
           Json(JsonObject{{"type", "integer"}}),
       }},
  });
  // 1 matches both number and integer -> oneOf should fail
  auto errs_one = validate_all(Json(1.0), one_schema);
  assert(!errs_one.empty());
}

static void test_schema_format_and_multipleof_and_conditionals() {
  // format
  validate(Json("a@b.com"), Json(JsonObject{{"type", "string"}, {"format", "email"}}));
  assert(!validate_all(Json("nope"), Json(JsonObject{{"type", "string"}, {"format", "email"}})).empty());

  validate(Json("550e8400-e29b-41d4-a716-446655440000"),
           Json(JsonObject{{"type", "string"}, {"format", "uuid"}}));
  assert(!validate_all(Json("not-a-uuid"), Json(JsonObject{{"type", "string"}, {"format", "uuid"}})).empty());

  validate(Json("2020-01-02T03:04:05Z"), Json(JsonObject{{"type", "string"}, {"format", "date-time"}}));
  assert(!validate_all(Json("2020-01-02"), Json(JsonObject{{"type", "string"}, {"format", "date-time"}})).empty());

  // multipleOf
  validate(Json(0.3), Json(JsonObject{{"type", "number"}, {"multipleOf", 0.1}}));
  assert(!validate_all(Json(0.35), Json(JsonObject{{"type", "number"}, {"multipleOf", 0.1}})).empty());

  // if/then/else
  Json schema_if;
  {
    JsonObject if_properties;
    if_properties["type"] = Json(JsonObject{{"const", Json("a")}});
    JsonObject if_schema;
    if_schema["properties"] = Json(if_properties);

    JsonObject then_schema;
    then_schema["required"] = Json(JsonArray{Json("aVal")});

    JsonObject else_schema;
    else_schema["required"] = Json(JsonArray{Json("bVal")});

    schema_if = Json(JsonObject{
        {"type", "object"},
        {"if", Json(if_schema)},
        {"then", Json(then_schema)},
        {"else", Json(else_schema)},
    });
  }
  {
    auto errs = validate_all(loads_jsonish("{\"type\":\"a\"}"), schema_if);
    assert(!errs.empty());
  }
  {
    auto errs = validate_all(loads_jsonish("{\"type\":\"b\"}"), schema_if);
    assert(!errs.empty());
  }
}

static void test_schema_dependent_required_contains_and_property_names() {
  // dependentRequired
  Json dep = Json(JsonObject{
      {"type", "object"},
      {"dependentRequired", Json(JsonObject{{"a", JsonArray{Json("b")}}})},
  });
  validate(loads_jsonish("{\"a\":1,\"b\":2}"), dep);
  {
    auto errs = validate_all(loads_jsonish("{\"a\":1}"), dep);
    assert(!errs.empty());
    assert(has_error_path(errs, "$.b"));
  }

  // contains + minContains/maxContains
  Json contains = Json(JsonObject{
      {"type", "array"},
      {"contains", Json(JsonObject{{"type", "integer"}})},
      {"minContains", 2.0},
      {"maxContains", 2.0},
  });
  validate(loads_jsonish("[1,2]"), contains);
  assert(!validate_all(loads_jsonish("[1]"), contains).empty());
  assert(!validate_all(loads_jsonish("[1,2,3]"), contains).empty());

  // propertyNames
  Json pn = Json(JsonObject{
      {"type", "object"},
      {"propertyNames", Json(JsonObject{{"type", "string"}, {"pattern", "^[a-z]+$"}})},
  });
  validate(loads_jsonish("{\"ok\":1}"), pn);
  assert(!validate_all(loads_jsonish("{\"Bad\":1}"), pn).empty());
}

static void test_repair_config_and_metadata() {
  RepairConfig rc;
  rc.allow_single_quotes = true;
  auto r = loads_jsonish_ex("```json\n{'a': True,}\n```", rc);
  assert(r.value.is_object());
  assert(r.value.as_object().at("a").is_bool());
  assert(r.metadata.extracted_from_fence);
  assert(r.metadata.replaced_python_literals);
  assert(r.metadata.dropped_trailing_commas);

  RepairConfig strict;
  strict.allow_single_quotes = false;
  try {
    (void)loads_jsonish_ex("{'a': 1}", strict);
    assert(false && "expected ValidationError");
  } catch (const ValidationError& e) {
    assert(e.kind == "parse");
  }
}

static void test_stream_finish_and_validated_batch_defaults() {
  Json obj_schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}, {"default", 1.0}})}})},
  });

  // finish() should turn an incomplete stream into a parse error
  {
    JsonStreamParser p(obj_schema);
    p.append("```json\n{");
    p.finish();
    auto out = p.poll();
    assert(out.done);
    assert(!out.ok);
    assert(out.error.has_value());
    assert(out.error->kind == "parse");
    assert(out.error->path == "$.stream.incomplete");
  }

  // Validated per-item batch collector applies defaults
  {
    JsonStreamValidatedBatchCollector c(obj_schema);
    c.append("{}\n");
    auto o1 = c.poll();
    assert(!o1.done);
    assert(o1.ok);
    assert(o1.value.has_value());
    assert(o1.value->size() == 1);
    assert((*o1.value)[0].as_object().at("age").as_number() == 1);
    c.close();
    auto o2 = c.poll();
    assert(o2.done);
    assert(o2.ok);
  }
}

static void test_sql_safety_hardening() {
  // Alias-aware allowedColumns
  {
    std::string sql = "SELECT u.id FROM users u WHERE u.id = ? LIMIT 1";
    Json schema = Json(JsonObject{
        {"allowedStatements", JsonArray{Json("select")}},
        {"requireLimit", Json(true)},
        {"allowedTables", JsonArray{Json("users")}},
        {"allowedColumns", Json(JsonObject{{"users", JsonArray{Json("id")}}})},
        {"allowUnqualifiedColumns", Json(false)},
        {"placeholderStyle", Json("qmark")},
    });
    (void)parse_and_validate_sql(sql, schema);
  }
  {
    std::string sql = "SELECT u.name FROM users u WHERE u.id = ? LIMIT 1";
    Json schema = Json(JsonObject{
        {"allowedStatements", JsonArray{Json("select")}},
        {"requireLimit", Json(true)},
        {"allowedTables", JsonArray{Json("users")}},
        {"allowedColumns", Json(JsonObject{{"users", JsonArray{Json("id")}}})},
        {"placeholderStyle", Json("qmark")},
    });
    try {
      (void)parse_and_validate_sql(sql, schema);
      assert(false && "expected ValidationError");
    } catch (const ValidationError& e) {
      assert(e.path.find("$.columns[") == 0);
    }
  }

  // forbidFunctions
  {
    std::string sql = "SELECT count(id) FROM users LIMIT 1";
    Json schema = Json(JsonObject{{"forbidFunctions", JsonArray{Json("count")}}});
    try {
      (void)parse_and_validate_sql(sql, schema);
      assert(false && "expected ValidationError");
    } catch (const ValidationError& e) {
      assert(e.path.find("$.functions[") == 0);
    }
  }

  // maxJoins
  {
    std::string sql = "SELECT u.id FROM users u JOIN orders o ON o.user_id = u.id LIMIT 1";
    Json schema = Json(JsonObject{{"maxJoins", 0.0}});
    try {
      (void)parse_and_validate_sql(sql, schema);
      assert(false && "expected ValidationError");
    } catch (const ValidationError& e) {
      assert(e.path == "$.joins.count");
    }
  }

  // placeholder style dollar + forbidOrTrue
  {
    std::string sql = "SELECT id FROM users WHERE id = $1 OR 1=1 LIMIT 1";
    Json schema = Json(JsonObject{{"placeholderStyle", Json("dollar")}, {"forbidOrTrue", Json(true)}});
    try {
      (void)parse_and_validate_sql(sql, schema);
      assert(false && "expected ValidationError");
    } catch (const ValidationError& e) {
      assert(e.path == "$.where.orTrue");
    }
  }
}

static void test_json_stream_batch_collector_emits() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamBatchCollector c(schema);
  c.append("{\"age\": 1}\n");
  auto o1 = c.poll();
  assert(!o1.done);
  assert(o1.ok);
  assert(o1.value.has_value());
  assert(o1.value->size() == 1);

  // No new item yet
  auto o2 = c.poll();
  assert(!o2.done);
  assert(!o2.ok);

  c.append("{\"age\": 2}\n");
  auto o3 = c.poll();
  assert(!o3.done);
  assert(o3.ok);
  assert(o3.value.has_value());
  assert(o3.value->size() == 1);

  c.close();
  auto o4 = c.poll();
  assert(o4.done);
  assert(o4.ok);
}

static void test_json_stream_batch_collector_max_items() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"required", JsonArray{Json("age")}},
      {"properties", Json(JsonObject{{"age", Json(JsonObject{{"type", "integer"}})}})},
  });

  JsonStreamBatchCollector c(schema, 0, 1);
  c.append("{\"age\": 1}\n");
  auto o1 = c.poll();
  assert(!o1.done);
  assert(o1.ok);
  assert(o1.value.has_value());
  assert(o1.value->size() == 1);

  c.append("{\"age\": 2}\n");
  auto o2 = c.poll();
  assert(o2.done);
  assert(!o2.ok);
  assert(o2.error.has_value());
  assert(o2.error->path == "$.stream.maxItems");
  assert(std::string(o2.error->what()).find("max=1") != std::string::npos);
}

static void test_schema_object_constraints_min_max_properties() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"minProperties", 2.0},
      {"maxProperties", 2.0},
      {"properties",
       Json(JsonObject{
           {"a", Json(JsonObject{{"type", "integer"}})},
           {"b", Json(JsonObject{{"type", "integer"}})},
       })},
  });

  // Too few
  auto e1 = validate_all(loads_jsonish("{\"a\": 1}"), schema);
  assert(!e1.empty());

  // Too many
  auto e2 = validate_all(loads_jsonish("{\"a\": 1, \"b\": 2, \"c\": 3}"), schema);
  assert(!e2.empty());

  // Exactly 2
  validate(loads_jsonish("{\"a\": 1, \"b\": 2}"), schema);
}

static void test_schema_string_pattern() {
  Json schema = Json(JsonObject{
      {"type", "string"},
      {"pattern", "^a.+z$"},
  });
  validate(Json("abz"), schema);
  auto errs = validate_all(Json("nope"), schema);
  assert(!errs.empty());
}

static void test_schema_const_keyword() {
  Json schema = Json(JsonObject{{"const", Json("ok")}});
  validate(Json("ok"), schema);
  auto errs = validate_all(Json("no"), schema);
  assert(!errs.empty());
}

static void test_schema_allof_keyword() {
  Json schema = Json(JsonObject{
      {"allOf",
       JsonArray{
           Json(JsonObject{{"type", "integer"}}),
           Json(JsonObject{{"minimum", 0.0}}),
       }},
  });

  validate(Json(1.0), schema);
  auto errs = validate_all(Json(-1.0), schema);
  assert(!errs.empty());
}

static void test_additional_properties_schema_is_enforced() {
  Json schema = Json(JsonObject{
      {"type", "object"},
      {"properties", Json(JsonObject{{"a", Json(JsonObject{{"type", "integer"}})}})},
      {"additionalProperties", Json(JsonObject{{"type", "string"}, {"minLength", 1.0}})},
  });

  // extra is allowed but must be a non-empty string
  validate(loads_jsonish("{\"a\": 1, \"extra\": \"x\"}"), schema);

  auto errs = validate_all(loads_jsonish("{\"a\": 1, \"extra\": 2}"), schema);
  assert(!errs.empty());
}

int main() {
  auto run = [](const char* name, void (*fn)()) {
    try {
      fn();
      std::cout << "PASS: " << name << "\n";
    } catch (const std::exception& e) {
      std::cerr << "FAIL: " << name << ": " << e.what() << "\n";
      throw;
    }
  };

  try {
    run("json", test_json_extract_and_validate);
    run("json_missing_required_path", test_json_missing_required_path);
    run("json_duplicate_key_policy", test_json_duplicate_key_policy);
    run("markdown", test_markdown_validate);
    run("markdown_missing_heading_path", test_markdown_missing_heading_path);
    run("markdown_line_too_long_path", test_markdown_line_too_long_path);
    run("kv", test_kv_validate);
    run("kv_missing_required_path", test_kv_missing_required_path);
    run("sql", test_sql_validate);
    run("sql_missing_limit_path", test_sql_missing_limit_path);
    run("json_stream_parser_success", test_json_stream_parser_success);
    run("json_stream_parser_error", test_json_stream_parser_error);
    run("json_stream_parser_max_buffer_bytes", test_json_stream_parser_max_buffer_bytes);
    run("sql_stream_parser_success", test_sql_stream_parser_success);
    run("sql_stream_parser_error", test_sql_stream_parser_error);
    run("sql_stream_parser_max_buffer_bytes", test_sql_stream_parser_max_buffer_bytes);
    run("json_stream_collector_all_success", test_json_stream_collector_all_success);
    run("json_stream_collector_all_error", test_json_stream_collector_all_error);
    run("json_stream_collector_max_items", test_json_stream_collector_max_items);
    run("validate_all_collects_multiple_errors", test_validate_all_collects_multiple_errors);
    run("parse_and_validate_with_defaults_fills", test_parse_and_validate_with_defaults_fills);
    run("schema_combinators_anyof_oneof", test_schema_combinators_anyof_oneof);
    run("schema_format_multipleof_conditionals", test_schema_format_and_multipleof_and_conditionals);
    run("schema_dependent_contains_property_names", test_schema_dependent_required_contains_and_property_names);
    run("repair_config_and_metadata", test_repair_config_and_metadata);
    run("json_stream_batch_collector_emits", test_json_stream_batch_collector_emits);
    run("json_stream_batch_collector_max_items", test_json_stream_batch_collector_max_items);
    run("stream_finish_and_validated_batch_defaults", test_stream_finish_and_validated_batch_defaults);
    run("sql_safety_hardening", test_sql_safety_hardening);
    run("schema_object_constraints_min_max_properties", test_schema_object_constraints_min_max_properties);
    run("schema_string_pattern", test_schema_string_pattern);
    run("schema_const_keyword", test_schema_const_keyword);
    run("schema_allof_keyword", test_schema_allof_keyword);
    run("additional_properties_schema_is_enforced", test_additional_properties_schema_is_enforced);
    std::cout << "OK\n";
    return 0;
  } catch (...) {
    return 1;
  }
}
