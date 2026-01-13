import {
	parseAndRepair,
	validateWithRepair,
	type JsonSchema,
} from "../src/index";

function main(): void {
	const schema: JsonSchema = {
		type: "object",
		required: ["name", "age"],
		additionalProperties: false,
		properties: {
			name: { type: "string", minLength: 1 },
			age: { type: "integer", minimum: 0, maximum: 120 },
		},
	};

	// 1) Validate an already-parsed JS value and get repair suggestions.
	const value: any = { name: "  Alice  ", age: "200", extra: true };
	const r1 = validateWithRepair(value, schema, {
		coerceTypes: true,
		clampNumbers: true,
		removeExtraProperties: true,
	});

	console.log("--- validateWithRepair(value, schema) ---");
	console.log({ valid: r1.valid, fullyRepaired: r1.fullyRepaired, repairedValue: r1.repairedValue });
	console.log("suggestions:");
	for (const s of r1.suggestions) {
		console.log(" -", s.path, s.errorKind, "->", s.suggestion);
	}

	// 2) Parse messy LLM output and auto-repair to the schema.
	const llmOutput = `
下面是结果（严格 JSON）：

\`\`\`json
{
  "name": "  Bob  ",
  "age": "42",
  "extra": "please remove",
}
\`\`\`
`;

	const r2 = parseAndRepair(llmOutput, schema, {
		coerceTypes: true,
		clampNumbers: true,
		removeExtraProperties: true,
	});

	console.log("\n--- parseAndRepair(text, schema) ---");
	console.log({ valid: r2.valid, fullyRepaired: r2.fullyRepaired, repairedValue: r2.repairedValue });
	console.log("unfixableErrors:", r2.unfixableErrors.length);
}

main();
