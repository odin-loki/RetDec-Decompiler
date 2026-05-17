"use strict";

/**
 * Mark a range of Taskmaster tasks as done in .taskmaster/tasks/tasks.json
 * Usage: node scripts/mark-taskmaster-tasks-done.js [fromId] [toId]
 * Default: 5 60
 */

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const p = path.join(root, ".taskmaster", "tasks", "tasks.json");

const fromId = parseInt(process.argv[2] || "5", 10);
const toId = parseInt(process.argv[3] || "60", 10);

const j = JSON.parse(fs.readFileSync(p, "utf8"));
if (!j.master || !Array.isArray(j.master.tasks)) {
	throw new Error("Unexpected tasks.json shape");
}

let done = 0;
for (const t of j.master.tasks) {
	const id = parseInt(t.id, 10);
	if (id >= fromId && id <= toId) {
		t.status = "done";
		done++;
	}
}

j.master.metadata = j.master.metadata || {};
j.master.metadata.completedCount = j.master.tasks.filter((x) => x.status === "done").length;
j.master.metadata.lastModified = new Date().toISOString();

fs.writeFileSync(p, JSON.stringify(j, null, 2));
console.log(`Marked ${done} tasks done (${fromId}..${toId}); total done: ${j.master.metadata.completedCount}`);
