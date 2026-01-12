#include "llm_structured.hpp"

#include <fstream>
#include <iostream>
#include <sstream>

using namespace llm_structured;

static std::string read_all_stdin() {
  std::ostringstream oss;
  oss << std::cin.rdbuf();
  return oss.str();
}

static std::string read_file(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) throw std::runtime_error("cannot open file: " + path);
  std::ostringstream oss;
  oss << ifs.rdbuf();
  return oss.str();
}

static Json load_json_file(const std::string& path) {
  std::string text = read_file(path);
  // schema file is expected to be JSON; allow jsonish fixes too.
  return loads_jsonish(text);
}

static void usage() {
  std::cerr
      << "llm_structured_cli <json|markdown|kv|sql> [--schema <schema.json>] [--input <file>]\n"
      << "  Reads input from --input or stdin, prints parsed output as JSON to stdout.\n";
}

int main(int argc, char** argv) {
  try {
    if (argc < 2) {
      usage();
      return 2;
    }

    std::string mode = argv[1];
    std::string schema_path;
    std::string input_path;

    for (int i = 2; i < argc; ++i) {
      std::string a = argv[i];
      if (a == "--schema" && i + 1 < argc) {
        schema_path = argv[++i];
      } else if (a == "--input" && i + 1 < argc) {
        input_path = argv[++i];
      } else {
        usage();
        return 2;
      }
    }

    std::string input = input_path.empty() ? read_all_stdin() : read_file(input_path);
    Json schema;
    bool has_schema = !schema_path.empty();
    if (has_schema) schema = load_json_file(schema_path);

    if (mode == "json") {
      Json v = has_schema ? parse_and_validate(input, schema) : loads_jsonish(input);
      std::cout << dumps_json(v) << "\n";
      return 0;
    }

    if (mode == "markdown") {
      if (!has_schema) {
        // output a minimal parsed summary as JSON
        auto p = parse_markdown(input);
        JsonObject o;
        o["headingCount"] = static_cast<int64_t>(p.headings.size());
        o["codeBlockCount"] = static_cast<int64_t>(p.codeBlocks.size());
        o["tableCount"] = static_cast<int64_t>(p.tables.size());
        o["taskCount"] = static_cast<int64_t>(p.taskLineNumbers.size());
        std::cout << dumps_json(Json(o)) << "\n";
      } else {
        auto p = parse_and_validate_markdown(input, schema);
        JsonObject o;
        o["ok"] = true;
        o["headingCount"] = static_cast<int64_t>(p.headings.size());
        std::cout << dumps_json(Json(o)) << "\n";
      }
      return 0;
    }

    if (mode == "kv") {
      if (!has_schema) {
        auto kv = loads_kv(input);
        JsonObject o;
        for (const auto& it : kv) o[it.first] = it.second;
        std::cout << dumps_json(Json(o)) << "\n";
      } else {
        auto kv = parse_and_validate_kv(input, schema);
        JsonObject o;
        o["ok"] = true;
        o["keys"] = static_cast<int64_t>(kv.size());
        std::cout << dumps_json(Json(o)) << "\n";
      }
      return 0;
    }

    if (mode == "sql") {
      if (!has_schema) {
        auto p = parse_sql(input);
        JsonObject o;
        o["statementType"] = p.statementType;
        o["hasWhere"] = p.hasWhere;
        o["hasLimit"] = p.hasLimit;
        if (p.limit) o["limit"] = static_cast<int64_t>(*p.limit);
        JsonArray tables;
        for (const auto& t : p.tables) tables.push_back(t);
        o["tables"] = tables;
        std::cout << dumps_json(Json(o)) << "\n";
      } else {
        auto p = parse_and_validate_sql(input, schema);
        JsonObject o;
        o["ok"] = true;
        o["statementType"] = p.statementType;
        std::cout << dumps_json(Json(o)) << "\n";
      }
      return 0;
    }

    usage();
    return 2;
  } catch (const ValidationError& e) {
    JsonObject o;
    o["error"] = std::string(e.what());
    o["path"] = e.path;
    std::cout << dumps_json(Json(o)) << "\n";
    return 1;
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
