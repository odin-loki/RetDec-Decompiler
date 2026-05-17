"use strict";

/**
 * Overwrite .taskmaster/tasks/tasks.json from repo-root tasks.json (flat { tasks: [...] }).
 * Converts numeric id/deps to strings for Taskmaster's tagged format.
 *
 * Usage (from repo root): node scripts/sync-tasks-json-to-taskmaster.js
 */

const fs = require("fs");
const path = require("path");

const rootDir = path.resolve(__dirname, "..");
const src = path.join(rootDir, "tasks.json");
const dst = path.join(rootDir, ".taskmaster", "tasks", "tasks.json");

const root = JSON.parse(fs.readFileSync(src, "utf8"));
if (!root.tasks || !Array.isArray(root.tasks)) {
	throw new Error(`${src} must contain a "tasks" array`);
}

const masterTasks = root.tasks.map((t) => ({
	id: String(t.id),
	title: t.title,
	description: t.description,
	details: t.details,
	testStrategy: t.testStrategy,
	priority: t.priority,
	dependencies: (t.dependencies || []).map(String),
	status: t.status || "pending",
	subtasks: t.subtasks || [],
}));

const completedCount = masterTasks.filter((x) => x.status === "done").length;

const out = {
	master: {
		tasks: masterTasks,
		metadata: {
			version: "1.0.0",
			lastModified: new Date().toISOString(),
			taskCount: masterTasks.length,
			completedCount,
			tags: ["master"],
		},
	},
};

fs.mkdirSync(path.dirname(dst), { recursive: true });
fs.writeFileSync(dst, JSON.stringify(out, null, 2));
console.log(`Synced ${masterTasks.length} tasks -> ${path.relative(rootDir, dst)} (${completedCount} done)`);
