import assert from "node:assert/strict";

import {
  parseAndValidateJson,
  parseAndValidateJsonWithDefaults,
  loadsJsonishEx,
  extractJsonCandidates,
  loadsJsonishAll,
  loadsJsonishAllEx,
  parseAndValidateJsonEx,
  parseAndValidateJsonAll,
  parseAndValidateJsonWithDefaultsEx,
  validateAllJson,
  validateAllJsonValue,
  validateWithRepair,
  parseAndRepair,
  buildOpenaiFunctionTool,
  buildAnthropicTool,
  buildGeminiFunctionDeclaration,
  parseOpenaiToolCall,
  parseAnthropicToolUse,
  parseGeminiFunctionCall,
  parseOpenaiToolCallsFromResponse,
  parseAnthropicToolUsesFromResponse,
  parseGeminiFunctionCallsFromResponse,
  parseAndValidateKv,
  parseAndValidateMarkdown,
  parseAndValidateSql,
  extractXmlCandidate,
  loadsXmlEx,
  loadsHtmlEx,
  loadsXmlAsJson,
  dumpsXml,
  dumpsHtml,
  queryXml,
  xmlGetAttribute,
  xmlTextContent,
  validateXml,
  JsonStreamParser,
  JsonStreamCollector,
  JsonStreamBatchCollector,
  JsonStreamValidatedBatchCollector,
  SqlStreamParser,
  type JsonSchema,
  type KeyValueSchema,
  type MarkdownValidationSchema,
  type SqlValidationSchema,
  type XmlSchema,
} from "../src/index";

function testJsonSchemaKeywords(): void {
  const schema: JsonSchema = {
    allOf: [
      { type: "object", required: ["a"], properties: { a: { type: "integer" } } },
      { type: "object", required: ["b"], properties: { b: { type: "string", pattern: "^x" } } },
    ],
  };

  const ok = parseAndValidateJson('{"a":1,"b":"x1"}', schema) as any;
  assert.equal(ok.a, 1);

  const errs = validateAllJson('{"a":1,"b":"no"}', schema);
  assert.ok(errs.length >= 1);
}

function testJsonSchemaNewKeywords(): void {
  // format + multipleOf
  const ok = parseAndValidateJson('{"email":"a@b.com","n":2.5}', {
    type: "object",
    required: ["email", "n"],
    properties: {
      email: { type: "string", format: "email" },
      n: { type: "number", multipleOf: 0.5 },
    },
  }) as any;
  assert.equal(ok.email, "a@b.com");

  const errs1 = validateAllJson('{"email":"not","n":2.7}', {
    type: "object",
    required: ["email", "n"],
    properties: {
      email: { type: "string", format: "email" },
      n: { type: "number", multipleOf: 0.5 },
    },
  });
  assert.ok(errs1.length >= 1);

  // if/then/else + dependentRequired
  const schema2: JsonSchema = {
    type: "object",
    properties: {
      type: { type: "string", enum: ["a", "b"] },
      a: { type: "string" },
      b: { type: "string" },
    },
    if: { properties: { type: { const: "a" } }, required: ["type"] },
    then: { required: ["a"] },
    else: { required: ["b"] },
    dependentRequired: { a: ["type"] },
  };

  parseAndValidateJson('{"type":"a","a":"x"}', schema2);
  const errs2 = validateAllJson('{"type":"a"}', schema2);
  assert.ok(errs2.length >= 1);

  // contains + min/maxContains
  const errs3 = validateAllJson('{"xs":[1,2,3]}', {
    type: "object",
    required: ["xs"],
    properties: {
      xs: { type: "array", contains: { const: 2 }, minContains: 2 },
    },
  });
  assert.ok(errs3.length >= 1);

  // propertyNames
  const errs4 = validateAllJson('{"bad-key":1}', {
    type: "object",
    propertyNames: { type: "string", pattern: "^[A-Za-z_][A-Za-z0-9_]*$" },
  });
  assert.ok(errs4.length >= 1);
}

function testDefaults(): void {
  const schema: JsonSchema = {
    type: "object",
    required: ["name", "age"],
    properties: {
      name: { type: "string" },
      age: { type: "integer", default: 18 },
    },
  };

  const v = parseAndValidateJsonWithDefaults('{"name":"Ada"}', schema) as any;
  assert.equal(v.age, 18);
}

function testRepairExAndMetadata(): void {
  const r = loadsJsonishEx("```json\n{\"a\":1,}\n```", { dropTrailingCommas: true });
  assert.equal((r.value as any).a, 1);
  assert.equal(r.metadata.extractedFromFence, true);
  assert.equal(r.metadata.droppedTrailingCommas, true);
  assert.equal(typeof r.metadata.duplicateKeyCount, "number");

  const s: JsonSchema = { type: "object", required: ["a"], properties: { a: { type: "integer" } } };
  const r2 = parseAndValidateJsonEx("{\"a\":1,}", s, { dropTrailingCommas: true });
  assert.equal((r2.value as any).a, 1);

  const r3 = parseAndValidateJsonWithDefaultsEx("{\"name\":\"Ada\"}", {
    type: "object",
    required: ["name", "age"],
    properties: { name: { type: "string" }, age: { type: "integer", default: 18 } },
  });
  assert.equal((r3.value as any).age, 18);

  assert.throws(
    () => loadsJsonishEx("{'a':1}", { allowSingleQuotes: false }),
    (e) => (e as any).kind === "parse"
  );

  // Duplicate key policy
  const d1 = loadsJsonishEx('{"a":1,"a":2}', { duplicateKeyPolicy: "firstWins" });
  assert.equal((d1.value as any).a, 1);
  assert.equal(d1.metadata.duplicateKeyCount, 1);
  assert.equal(d1.metadata.duplicateKeyPolicy, "firstWins");

  const d2 = loadsJsonishEx('{"a":1,"a":2}', { duplicateKeyPolicy: "lastWins" });
  assert.equal((d2.value as any).a, 2);
  assert.equal(d2.metadata.duplicateKeyCount, 1);
  assert.equal(d2.metadata.duplicateKeyPolicy, "lastWins");

  assert.throws(
    () => loadsJsonishEx('{"a":1,"a":2}', { duplicateKeyPolicy: "error" }),
    (e) => (e as any).kind === "parse" && (e as any).path === "$.a"
  );
}

function testMultiJsonBlocks(): void {
  const text =
    "prefix\n" +
    "```json\n" +
    "{\"a\": 1}\n" +
    "```\n" +
    "middle {\"b\": 2} tail\n" +
    "```json\n" +
    "[1, 2]\n" +
    "```\n";

  const cands = extractJsonCandidates(text);
  assert.deepEqual(cands, ['{"a": 1}', '{"b": 2}', '[1, 2]']);

  const values = loadsJsonishAll(text);
  assert.deepEqual(values, [{ a: 1 }, { b: 2 }, [1, 2]]);

  const r = loadsJsonishAllEx(text);
  assert.equal(r.values.length, 3);
  assert.equal(r.fixed.length, 3);
  assert.equal(r.metadata.length, 3);
  assert.equal(r.metadata[0].extractedFromFence, true);
  assert.equal(r.metadata[1].extractedFromFence, false);

  assert.throws(
    () => parseAndValidateJsonAll('{"a":"x"} {"a":2}', { type: "object", required: ["a"], properties: { a: { type: "integer" } } }),
    (e) => (e as any).kind === "type" && (e as any).path === "$[0].a"
  );

  const ok = parseAndValidateJsonAll('{"a":1} {"a":2}', { type: "object", required: ["a"], properties: { a: { type: "integer" } } });
  assert.deepEqual(ok, [{ a: 1 }, { a: 2 }]);
}

function testValidateAllJsonValuePointers(): void {
  const errs = validateAllJsonValue({ items: [{ id: "x" }] } as any, {
    type: "object",
    required: ["items"],
    properties: {
      items: { type: "array", items: { type: "object", required: ["id"], properties: { id: { type: "integer" } } } },
    },
  });
  assert.ok(errs.length >= 1);
  assert.ok(errs.some((e) => e.path.includes("$.items[0].id")));
}

function testValidateWithRepairApis(): void {
  const schema: JsonSchema = {
    type: "object",
    required: ["age", "name"],
    additionalProperties: false,
    properties: {
      age: { type: "integer", minimum: 0, maximum: 120 },
      name: { type: "string", minLength: 1 },
    },
  };

  const r = validateWithRepair({ age: "200", name: "  Alice  ", extra: 1 } as any, schema, {
    coerceTypes: true,
    clampNumbers: true,
    removeExtraProperties: true,
    maxSuggestions: 20,
  });

  assert.equal(r.valid, false);
  assert.equal(r.fullyRepaired, true);
  assert.deepEqual(r.repairedValue, { age: 120, name: "  Alice  " });
  assert.ok(r.suggestions.length >= 1);
  assert.equal(r.unfixableErrors.length, 0);

  const r2 = parseAndRepair('{"age":"200","name":"Bob","extra":1}', schema, {
    coerceTypes: true,
    clampNumbers: true,
    removeExtraProperties: true,
  });
  assert.equal(r2.fullyRepaired, true);
  assert.deepEqual(r2.repairedValue, { age: 120, name: "Bob" });
}

function testXmlHtmlApis(): void {
  const llmText = `
下面是 XML：

\`\`\`xml
<items>
  <item id=a1> Hello </item>
  <item id=b2>World</item>
</items>
\`\`\`
`;

  const xml = extractXmlCandidate(llmText);
  assert.ok(xml.includes("<items>"));
  assert.ok(xml.includes("<item"));

  const parsed = loadsXmlEx(xml, {
    fix_unquoted_attributes: true,
    auto_close_tags: true,
    decode_entities: true,
  });
  assert.equal(parsed.ok, true);
  assert.equal(parsed.error, "");
  assert.ok(parsed.root);
  assert.ok(parsed.metadata.fixed_attributes >= 1);

  const items = queryXml(parsed.root!, "item");
  assert.equal(items.length, 2);
  assert.equal(xmlGetAttribute(items[0], "id"), "a1");
  assert.equal(xmlGetAttribute(items[1], "id"), "b2");
  assert.ok(xmlTextContent(items[0]).includes("Hello"));
  assert.ok(xmlTextContent(items[1]).includes("World"));

  const xmlOut = dumpsXml(parsed.root!, 2);
  assert.ok(xmlOut.includes("<items>"));
  assert.ok(xmlOut.includes('id="a1"'));

  const asJson = loadsXmlAsJson(xml) as any;
  assert.equal(asJson["#name"], "items");
  assert.equal(asJson["#children"].length, 2);

  const schema: XmlSchema = {
    element: "items",
    children: { minItems: 1, required: ["item"] },
  };
  const vOk = validateXml(parsed.root!, schema);
  assert.equal(vOk.ok, true);
  assert.equal(vOk.errors.length, 0);

  const badItemSchema: XmlSchema = {
    element: "item",
    requiredAttributes: ["id", "missing"],
  };
  const vBad = validateXml(items[0], badItemSchema);
  assert.equal(vBad.ok, false);
  assert.equal(vBad.errors.length, 1);
  assert.equal(vBad.errors[0].path, "$");
  assert.ok(vBad.errors[0].message.includes("Missing required attribute"));

  const html = `<div class=card><p>Hello<b>world</div>`;
  const htmlParsed = loadsHtmlEx(html, {
    html_mode: true,
    fix_unquoted_attributes: true,
    auto_close_tags: true,
    lowercase_names: true,
  });
  assert.equal(htmlParsed.ok, true);
  assert.ok(htmlParsed.root);
  const htmlOut = dumpsHtml(htmlParsed.root!, 2);
  assert.ok(htmlOut.includes("<div"));
  assert.ok(htmlOut.includes("<b>world</b>"));
}

function testToolCalling(): void {
  const schema: JsonSchema = {
    type: "object",
    additionalProperties: false,
    required: ["id"],
    properties: { id: { type: "integer" } },
  };

  const openai = buildOpenaiFunctionTool("get_user", "Get a user", schema);
  assert.equal((openai.tool as any).type, "function");
  assert.equal((openai.tool as any).function.name, "get_user");

  const anthropic = buildAnthropicTool("get_user", "Get a user", schema);
  assert.equal((anthropic.tool as any).name, "get_user");
  assert.ok((anthropic.tool as any).input_schema);

  const gemini = buildGeminiFunctionDeclaration("get_user", "Get a user", schema);
  assert.equal((gemini.tool as any).name, "get_user");
  assert.ok((gemini.tool as any).parameters);

  const toolCall = {
    id: "call_1",
    type: "function",
    function: { name: "get_user", arguments: "{'id': '123',}" },
  };
  const r1 = parseOpenaiToolCall(
    toolCall,
    schema,
    { coerceTypes: true },
    { allowSingleQuotes: true, dropTrailingCommas: true }
  );
  assert.equal(r1.platform, "openai");
  assert.equal(r1.name, "get_user");
  assert.equal(r1.ok, true);
  assert.deepEqual(r1.validation.repairedValue, { id: 123 });

  const toolUse = { type: "tool_use", id: "tu_1", name: "get_user", input: { id: "123", extra: 1 } };
  const r2 = parseAnthropicToolUse(toolUse, schema, { coerceTypes: true, removeExtraProperties: true });
  assert.equal(r2.platform, "anthropic");
  assert.equal(r2.ok, true);
  assert.deepEqual(r2.validation.repairedValue, { id: 123 });

  const functionCall = { name: "get_user", args: { id: "123" } };
  const r3 = parseGeminiFunctionCall(functionCall, schema, { coerceTypes: true });
  assert.equal(r3.platform, "gemini");
  assert.equal(r3.ok, true);
  assert.deepEqual(r3.validation.repairedValue, { id: 123 });

  const schemas = { get_user: schema };
  const openaiResp = { choices: [{ message: { tool_calls: [toolCall] } }] };
  const calls = parseOpenaiToolCallsFromResponse(openaiResp as any, schemas, { coerceTypes: true }, { allowSingleQuotes: true });
  assert.equal(calls.length, 1);
  assert.equal(calls[0].ok, true);

  const anthropicResp = {
    content: [
      { type: "text", text: "hi" },
      { type: "tool_use", id: "tu_1", name: "get_user", input: { id: "2" } },
    ],
  };
  const uses = parseAnthropicToolUsesFromResponse(anthropicResp as any, schemas, { coerceTypes: true });
  assert.equal(uses.length, 1);
  assert.equal(uses[0].ok, true);

  const geminiResp = {
    candidates: [
      {
        content: {
          parts: [{ functionCall: { name: "get_user", args: { id: "3" } } }],
        },
      },
    ],
  };
  const fcs = parseGeminiFunctionCallsFromResponse(geminiResp as any, schemas, { coerceTypes: true });
  assert.equal(fcs.length, 1);
  assert.equal(fcs[0].ok, true);

  const unknown = parseOpenaiToolCallsFromResponse(
    { tool_calls: [{ id: "call_x", type: "function", function: { name: "nope", arguments: "{}" } }] } as any,
    schemas
  );
  assert.equal(unknown.length, 1);
  assert.equal(unknown[0].ok, false);
  assert.ok(unknown[0].error.includes("unknown tool schema"));
}

function testKvAndMarkdown(): void {
  const kvSchema: KeyValueSchema = {
    required: ["A"],
    patterns: { A: "^\\d+$" },
    allowExtra: true,
  };
  const kv = parseAndValidateKv("A=1\nB=ok\n", kvSchema);
  assert.equal(kv.A, "1");

  const mdSchema: MarkdownValidationSchema = {
    requiredHeadings: ["Title"],
    requireTaskList: true,
  };
  const md = parseAndValidateMarkdown("# Title\n\n- [x] done\n", mdSchema);
  assert.ok(md.taskCount >= 1);
}

function testSqlStreamingAndLimits(): void {
  const schema: SqlValidationSchema = { allowedStatements: ["select"], requireLimit: true, forbidSemicolon: false };

  const sp = new SqlStreamParser(schema, { maxBufferBytes: 1024 });
  sp.append("SELECT 1 ");
  assert.equal(sp.poll().done, false);
  sp.append("LIMIT 1;");
  const out = sp.poll();
  assert.equal(out.done, true);
  assert.equal(out.ok, true);
  assert.equal(out.value?.limit, 1);

  const jp = new JsonStreamParser(
    { type: "object", required: ["age"], properties: { age: { type: "integer" } } },
    { maxBufferBytes: 8 }
  );
  jp.append("0123456789");
  const jout = jp.poll();
  assert.equal(jout.done, true);
  assert.equal(jout.ok, false);
  assert.equal(jout.error?.path, "$.stream.maxBufferBytes");
  assert.equal(jout.error?.limit?.kind, "maxBufferBytes");
}

function testStreamingFinishAndLocation(): void {
  const jp = new JsonStreamParser({ type: "object" });
  jp.append("{\n");
  const loc = jp.location();
  assert.ok(loc.offset >= 1);
  assert.ok(loc.line >= 1);
  assert.ok(loc.col >= 1);

  jp.finish();
  const out = jp.poll();
  assert.equal(out.done, true);
  assert.equal(out.ok, false);
  assert.equal(out.error?.kind, "parse");
  assert.ok(out.error?.path.includes("$.stream.incomplete"));
}

function testCollectors(): void {
  const schema: JsonSchema = { type: "object", required: ["age"], properties: { age: { type: "integer" } } };

  const c = new JsonStreamCollector(schema, { maxItems: 2 });
  c.append('{"age":1}\n');
  c.append('{"age":2}\n');
  c.close();
  const out = c.poll();
  assert.equal(out.done, true);
  assert.equal(out.ok, true);
  assert.equal(out.value?.length, 2);

  const bc = new JsonStreamBatchCollector(schema);
  bc.append('{"age":1}\n');
  const b1 = bc.poll();
  assert.equal(b1.done, false);
  assert.equal(b1.ok, true);
  assert.equal(b1.value?.length, 1);
  bc.close();
  const b2 = bc.poll();
  assert.equal(b2.done, true);
  assert.equal(b2.ok, true);

  const vbc = new JsonStreamValidatedBatchCollector({
    type: "object",
    required: ["name", "age"],
    properties: { name: { type: "string" }, age: { type: "integer", default: 18 } },
  });
  vbc.append('{"name":"Ada"}\n');
  const vb1 = vbc.poll();
  assert.equal(vb1.ok, true);
  assert.equal((vb1.value?.[0] as any).age, 18);
}

function testSqlHardening(): void {
  const schema: SqlValidationSchema = {
    allowedStatements: ["select"],
    allowedTables: ["users"],
    allowedColumns: ["u.id"],
    allowUnqualifiedColumns: false,
    maxJoins: 0,
    forbidFunctions: true,
    forbidOrTrue: true,
    placeholderStyle: "qmark",
    requireLimit: true,
  };

  const ok = parseAndValidateSql("SELECT u.id FROM users u WHERE u.id = ? LIMIT 1", schema);
  assert.equal(ok.limit, 1);

  assert.throws(
    () => parseAndValidateSql("SELECT COUNT(*) FROM users u WHERE u.id = ? LIMIT 1", schema),
    (e) => typeof (e as any).kind === "string"
  );
}

function main(): void {
  testJsonSchemaKeywords();
  testJsonSchemaNewKeywords();
  testDefaults();
  testRepairExAndMetadata();
  testMultiJsonBlocks();
  testValidateAllJsonValuePointers();
  testValidateWithRepairApis();
  testXmlHtmlApis();
  testToolCalling();
  testKvAndMarkdown();
  testSqlStreamingAndLimits();
  testStreamingFinishAndLocation();
  testCollectors();
  testSqlHardening();
  console.log("OK");
}

main();
