import {
	parseAndValidateJson,
	parseAndValidateJsonWithDefaults,
	parseAndValidateSql,
	parseAndValidateMarkdown,
	validateAllJson,
	validateAllJsonValue,
	JsonStreamParser,
	JsonStreamCollector,
	JsonStreamBatchCollector,
	SqlStreamParser,
	type JsonSchema,
	type SqlValidationSchema,
	type MarkdownValidationSchema,
} from "../src/index";

const jsonSchema: JsonSchema = {
	type: "object",
	required: ["name", "age"],
	additionalProperties: false,
	properties: {
		name: { type: "string", minLength: 1 },
		age: { type: "integer", minimum: 0 }
	}
};

console.log(parseAndValidateJson('```json\n{"name":"Ada","age":12,}\n```', jsonSchema));

// Defaults demo
{
	const withDefaults = parseAndValidateJsonWithDefaults('```json\n{"name":"Ada"}\n```', {
		type: "object",
		required: ["name", "age"],
		properties: {
			name: { type: "string" },
			age: { type: "integer", default: 18 },
		},
	});
	if ((withDefaults as any).age !== 18) throw new Error("expected default age=18");
}

// Collect-all validation demo
{
	const errs = validateAllJson('{"age": -1, "extra": 1}', {
		type: "object",
		required: ["name"],
		additionalProperties: false,
		properties: {
			name: { type: "string", minLength: 2 },
			age: { type: "integer", minimum: 0 },
		},
	});
	if (errs.length < 2) throw new Error("expected multiple validation errors");
	if (!errs.some((e) => e.path === "$.name" && e.jsonPointer === "/name")) {
		throw new Error("expected missing name error with jsonPointer");
	}
}

// Collect-all from already-parsed JS value
{
	const errs = validateAllJsonValue({ age: -1, extra: 1 } as any, {
		type: "object",
		required: ["name"],
		additionalProperties: false,
		properties: {
			name: { type: "string", minLength: 2 },
			age: { type: "integer", minimum: 0 },
		},
	});
	if (errs.length < 2) throw new Error("expected multiple validation errors from value");
}

const sqlSchema: SqlValidationSchema = {
	allowedStatements: ["select"],
	requireFrom: true,
	requireWhere: true,
	requireLimit: true,
	maxLimit: 10,
	forbidUnion: true,
	forbidSemicolon: true,
	forbidComments: true,
	forbidSelectStar: true,
	requireOrderBy: true,
	allowedTables: ["users"]
};

console.log(
	parseAndValidateSql(
		"SELECT id FROM users WHERE id = 1 ORDER BY id DESC LIMIT 1",
		sqlSchema
	)
);

try {
	parseAndValidateSql("SELECT id FROM users WHERE id = 1 ORDER BY id DESC", sqlSchema);
} catch (e: any) {
	console.log("Caught SQL error:", {
		name: e?.name,
		message: e?.message,
		path: e?.path,
	});
}

const mdSchema: MarkdownValidationSchema = {
	requiredHeadings: ["Intro"],
};

try {
	parseAndValidateMarkdown("# Title\n", mdSchema);
} catch (e: any) {
	console.log("Caught Markdown error:", {
		name: e?.name,
		message: e?.message,
		path: e?.path,
	});
}

// ---- Failure demo: show structured ValidationError with .path
try {
	// Missing required "age".
	parseAndValidateJson('```json\n{"name":"Ada"}\n```', jsonSchema);
} catch (e: any) {
	console.log("Caught error:", {
		name: e?.name,
		message: e?.message,
		path: e?.path,
	});
}

// ---- Streaming demos (these should be stable regressions; demo acts as `npm test`) ----
{
	const sp = new JsonStreamParser({
		type: "object",
		required: ["age"],
		properties: { age: { type: "integer" } },
	});

	sp.append("blah\n```json\n{");
	const o1 = sp.poll();
	if (o1.done) throw new Error("expected JSON stream not done after first chunk");
	sp.append('"age": 1}');
	const o2 = sp.poll();
	if (o2.done) throw new Error("expected JSON stream not done before closing fence");
	sp.append("\n```\n");
	const o3 = sp.poll();
	if (!o3.done || !o3.ok || !o3.value || (o3.value as any).age !== 1) {
		throw new Error("expected JSON stream done+ok with age=1");
	}
}

// Streaming limit: maxBufferBytes
{
	const sp = new JsonStreamParser(
		{
			type: "object",
			required: ["age"],
			properties: { age: { type: "integer" } },
		},
		{ maxBufferBytes: 8 }
	);
	sp.append("0123456789");
	const out = sp.poll();
	if (!out.done || out.ok || !out.error) {
		throw new Error("expected JSON stream to fail when exceeding maxBufferBytes");
	}
	if (out.error.path !== "$.stream.maxBufferBytes") {
		throw new Error("expected JSON stream buffer error path=$.stream.maxBufferBytes");
	}
	if (!out.error.limit || out.error.limit.kind !== "maxBufferBytes" || out.error.limit.max !== 8 || out.error.limit.current < 9) {
		throw new Error("expected JSON stream buffer error.limit={kind:maxBufferBytes,current,max}");
	}
}

{
	const sp = new SqlStreamParser({
		allowedStatements: ["select"],
		requireLimit: true,
	});

	sp.append("SELECT id FROM users ");
	if (sp.poll().done) throw new Error("expected SQL stream not done after first chunk");
	sp.append("WHERE id = 1 ");
	if (sp.poll().done) throw new Error("expected SQL stream not done before semicolon");
	sp.append("LIMIT 1;");
	const out = sp.poll();
	if (!out.done || !out.ok || !out.value || out.value.limit !== 1) {
		throw new Error("expected SQL stream done+ok with limit=1");
	}
}

{
	const c = new JsonStreamCollector({
		type: "object",
		required: ["age"],
		properties: { age: { type: "integer" } },
	});
	c.append('{"age": 1}\n');
	if (c.poll().done) throw new Error("expected collector not done before close");
	c.append('{"age": 2}\n');
	c.close();
	const out = c.poll();
	if (!out.done || !out.ok || !out.value || out.value.length !== 2) {
		throw new Error("expected collector done+ok with 2 items");
	}
	if ((out.value[0] as any).age !== 1 || (out.value[1] as any).age !== 2) {
		throw new Error("unexpected collector items");
	}
}

// Streaming limit: maxItems
{
	const c = new JsonStreamCollector(
		{
			type: "object",
			required: ["age"],
			properties: { age: { type: "integer" } },
		},
		{ maxItems: 1 }
	);
	c.append('{"age": 1}\n');
	c.append('{"age": 2}\n');
	c.close();
	const out = c.poll();
	if (!out.done || out.ok || !out.error) {
		throw new Error("expected collector to fail when exceeding maxItems");
	}
	if (out.error.path !== "$.stream.maxItems") {
		throw new Error("expected collector maxItems error path=$.stream.maxItems");
	}
	if (!out.error.limit || out.error.limit.kind !== "maxItems" || out.error.limit.max !== 1 || out.error.limit.current !== 2) {
		throw new Error("expected collector maxItems error.limit={kind:maxItems,current,max}");
	}
}

// Emit-all incrementally (no need to close to get first batch)
{
	const c = new JsonStreamBatchCollector({
		type: "object",
		required: ["age"],
		properties: { age: { type: "integer" } },
	});
	c.append('{"age": 1}\n');
	const b1 = c.poll();
	if (b1.done || !b1.ok || !b1.value || b1.value.length !== 1) {
		throw new Error("expected batch collector to emit first item");
	}
	c.append('{"age": 2}\n');
	const b2 = c.poll();
	if (b2.done || !b2.ok || !b2.value || b2.value.length !== 1) {
		throw new Error("expected batch collector to emit second item");
	}
	const b3 = c.poll();
	if (b3.done || b3.ok) throw new Error("expected no new items yet");
	c.close();
	const b4 = c.poll();
	if (!b4.done || !b4.ok) throw new Error("expected batch collector to finish after close");
}

{
	const sp = new SqlStreamParser({
		allowedStatements: ["select"],
		requireLimit: true,
	});
	sp.append("SELECT id FROM users WHERE id = 1;");
	const out = sp.poll();
	if (!out.done || out.ok || !out.error || out.error.path !== "$.limit") {
		throw new Error("expected SQL stream done+error with path=$.limit");
	}
	if (out.error.jsonPointer !== "/limit") {
		throw new Error("expected SQL stream error jsonPointer=/limit");
	}
}
