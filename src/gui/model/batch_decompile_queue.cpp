/**
 * @file src/gui/model/batch_decompile_queue.cpp
 * @copyright (c) 2026 Odin Loch trading as Imortek
 */

#include "retdec/gui/model/batch_decompile_queue.h"

#include <format>

namespace retdec::gui::model {

std::string batchDecompileStatusLabel(int index, int total, const std::filesystem::path& binaryPath)
{
	// Accept either separator whatever the host, as QFileInfo::fileName() did
	// for paths that came from a Windows machine (project files, batch lists).
	const std::string s = binaryPath.generic_string();
	const auto cut = s.find_last_of("/\\");
	const std::string name = cut == std::string::npos ? s : s.substr(cut + 1);
	return std::format("Batch {}/{}: {}", index, total, name);
}

int batchDecompileCurrentIndex(int total, int remainingCount)
{
	if (total <= 0 || remainingCount <= 0 || remainingCount > total) return 0;
	return total - remainingCount + 1;
}

void batchDecompilePopFront(std::vector<std::string>* queue)
{
	if (queue && !queue->empty()) queue->erase(queue->begin());
}

void batchDecompilePopFront(std::vector<std::filesystem::path>* queue)
{
	if (queue && !queue->empty()) queue->erase(queue->begin());
}

} // namespace retdec::gui::model
