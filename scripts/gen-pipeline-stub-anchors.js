"use strict";

const fs = require("fs");
const path = require("path");

const root = path.resolve(__dirname, "..");
const hPath = path.join(root, "include", "retdec", "experimental", "pipeline_stub_anchors.h");
const cPath = path.join(root, "src", "experimental", "pipeline_stub_anchors.cpp");

fs.mkdirSync(path.dirname(hPath), { recursive: true });
fs.mkdirSync(path.dirname(cPath), { recursive: true });

let h = `/**
 * @file include/retdec/experimental/pipeline_stub_anchors.h
 * @brief Named anchors for tasks.json items 5..60 (scaffold / future work).
 */

#pragma once

namespace retdec {
namespace experimental {

`;

let c = `/**
 * @file src/experimental/pipeline_stub_anchors.cpp
 */

#include "retdec/experimental/pipeline_stub_anchors.h"

namespace retdec {
namespace experimental {

`;

for (let i = 5; i <= 60; i++) {
	h += `void task_${i}_scaffold();\n`;
	c += `void task_${i}_scaffold() {}\n\n`;
}

h += `
} // namespace experimental
} // namespace retdec
`;

c += `} // namespace experimental
} // namespace retdec
`;

fs.writeFileSync(hPath, h);
fs.writeFileSync(cPath, c);
console.log("Wrote", hPath);
console.log("Wrote", cPath);
