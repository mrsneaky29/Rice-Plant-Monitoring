const fs = require("node:fs");
const path = require("node:path");
const html = fs.readFileSync(
  path.join(__dirname, "../PC_Hub/static/index.html"),
  "utf8",
);
const scripts = [...html.matchAll(/<script\b[^>]*>([\s\S]*?)<\/script>/gi)];
if (!scripts.length) throw new Error("Dashboard has no inline JavaScript");
for (const [, source] of scripts) new Function(source);
console.log("Dashboard JavaScript syntax is valid.");
