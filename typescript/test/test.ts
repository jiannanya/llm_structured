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
  parseAndValidateKv,
  parseAndValidateMarkdown,
  parseAndValidateSql,
  JsonStreamParser,
  JsonStreamCollector,
  JsonStreamBatchCollector,
  JsonStreamValidatedBatchCollector,
  SqlStreamParser,
  type JsonSchema,
  type KeyValueSchema,
  type MarkdownValidationSchema,
  type SqlValidationSchema,
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
  testKvAndMarkdown();
  testSqlStreamingAndLimits();
  testStreamingFinishAndLocation();
  testCollectors();
  testSqlHardening();
  console.log("OK");
}

main();
