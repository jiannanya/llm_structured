import {
	extractXmlCandidate,
	loadsXmlEx,
	loadsHtmlEx,
	dumpsXml,
	dumpsHtml,
	queryXml,
	xmlGetAttribute,
	xmlTextContent,
	validateXml,
	loadsXmlAsJson,
	type XmlSchema,
} from "../src/index";

function main(): void {
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
	console.log("--- Extracted XML ---");
	console.log(xml);

	const parsed = loadsXmlEx(xml, {
		fix_unquoted_attributes: true,
		auto_close_tags: true,
		decode_entities: true,
	});
	if (!parsed.ok || !parsed.root) throw new Error(parsed.error);

	console.log("\n--- loadsXmlEx metadata ---");
	console.log(parsed.metadata);

	console.log("\n--- queryXml(root, 'item') ---");
	const items = queryXml(parsed.root, "item");
	items.forEach((node, i) => {
		console.log(
			`item[${i}] id=${JSON.stringify(xmlGetAttribute(node, "id"))} text=${JSON.stringify(xmlTextContent(node))}`
		);
	});

	console.log("\n--- dumpsXml(root) ---");
	console.log(dumpsXml(parsed.root, 2));

	console.log("\n--- loadsXmlAsJson(xml) ---");
	console.log(JSON.stringify(loadsXmlAsJson(xml), null, 2));

	const schema: XmlSchema = {
		element: "items",
		children: { minItems: 1, required: ["item"] },
	};
	console.log("\n--- validateXml(root, schema) ---");
	console.log(validateXml(parsed.root, schema));

	// Failure demo: validate an <item> node directly and require an attribute that does not exist
	const badItemSchema: XmlSchema = {
		element: "item",
		requiredAttributes: ["id", "missing"],
	};
	console.log("\n--- validateXml(item[0], badItemSchema) (expected failure) ---");
	console.log(validateXml(items[0], badItemSchema));

	// HTML demo (lenient parsing)
	const html = `<div class=card><p>Hello<b>world</div>`;
	const htmlParsed = loadsHtmlEx(html, {
		html_mode: true,
		fix_unquoted_attributes: true,
		auto_close_tags: true,
		lowercase_names: true,
	});

	console.log("\n--- loadsHtmlEx metadata + dumpsHtml(root) ---");
	console.log(htmlParsed.metadata);
	if (htmlParsed.ok && htmlParsed.root) {
		console.log(dumpsHtml(htmlParsed.root, 2));
	}
}

main();
