"use strict";

/**
 * Set every task in .taskmaster/tasks/tasks.json and repo-root tasks.json to status "pending".
 * Usage (repo root): node scripts/reset-all-tasks-pending.js
 */

const fs = require("fs");
const path = require("path");

const rootDir = path.resolve(__dirname, "..");
const tmPath = path.join(rootDir, ".taskmaster", "tasks", "tasks.json");
const flatPath = path.join(rootDir, "tasks.json");

function resetTagged() {
	const j = JSON.parse(fs.readFileSync(tmPath, "utf8"));
	if (!j.master || !Array.isArray(j.master.tasks)) {
		throw new Error(`${tmPath}: expected master.tasks array`);
	}
	for (const t of j.master.tasks) {
		t.status = "pending";
		delete t.updatedAt;
	}
	j.master.metadata = j.master.metadata || {};
	j.master.metadata.completedCount = 0;
	j.master.metadata.lastModified = new Date().toISOString();
	j.master.metadata.taskCount = j.master.tasks.length;
	fs.writeFileSync(tmPath, JSON.stringify(j, null, 2));
	console.log(`Reset ${j.master.tasks.length} tasks to pending in ${path.relative(rootDir, tmPath)}`);
}

function resetFlat() {
	if (!fs.existsSync(flatPath)) {
		console.log("No root tasks.json; skipping.");
		return;
	}
	const j = JSON.parse(fs.readFileSync(flatPath, "utf8"));
	if (!j.tasks || !Array.isArray(j.tasks)) {
		throw new Error(`${flatPath}: expected tasks array`);
	}
	for (const t of j.tasks) {
		t.status = "pending";
	}
	fs.writeFileSync(flatPath, JSON.stringify(j, null, 2));
	console.log(`Reset ${j.tasks.length} tasks to pending in ${path.relative(rootDir, flatPath)}`);
}

resetTagged();
resetFlat();
console.log("Done. All tasks are pending in Taskmaster and in root tasks.json (if present).");
