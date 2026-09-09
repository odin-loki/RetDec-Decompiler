/**
 * @file src/config/config.cpp
 * @brief Decompilation configuration manipulation.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

#include <rapidjson/error/en.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/encodings.h>

#include "retdec/config/config.h"
#include "retdec/serdes/address.h"
#include "retdec/serdes/architecture.h"
#include "retdec/serdes/class.h"
#include "retdec/serdes/file_format.h"
#include "retdec/serdes/file_type.h"
#include "retdec/serdes/function.h"
#include "retdec/serdes/language.h"
#include "retdec/serdes/object.h"
#include "retdec/serdes/pattern.h"
#include "retdec/serdes/vtable.h"
#include "retdec/serdes/tool_info.h"
#include "retdec/serdes/type.h"
#include "retdec/utils/string.h"
#include "retdec/utils/time.h"

#include "retdec/serdes/std.h"

namespace {

const std::string JSON_date              = "date";
const std::string JSON_time              = "time";
const std::string JSON_parameters        = "decompParams";
const std::string JSON_architecture      = "architecture";
const std::string JSON_fileType          = "fileType";
const std::string JSON_fileFormat        = "fileFormat";
const std::string JSON_tools             = "tools";
const std::string JSON_functions         = "functions";
const std::string JSON_globals           = "globals";
const std::string JSON_registers         = "registers";
const std::string JSON_languages         = "languages";
const std::string JSON_structures        = "structures";
const std::string JSON_vtables           = "vtables";
const std::string JSON_classes           = "classes";
const std::string JSON_patterns          = "patterns";

} // anonymous namespace

namespace retdec {
namespace config {

namespace fs = std::filesystem;

/// A JSON configuration larger than this is not one. The bound exists so a
/// bogus size read back from a file that is not what it claimed cannot become
/// an allocation request.
static constexpr uint64_t kMaxConfigBytes = 1ULL << 30; // 1 GiB

Config Config::empty()
{
	Config config;
	return config;
}

Config Config::fromFile(const std::string& path)
{
	Config config;
	config.readJsonFile(path);
	return config;
}

Config Config::fromJsonString(const std::string& json)
{
	Config config;
	config.readJsonString(json);
	return config;
}

/**
 * Reads JSON file into internal representation.
 * If file can not be opened, an instance of @c FileNotFoundException is thrown.
 * If file can not be parsed, an instance of @c ParseException is thrown.
 * @param input Path to input JSON file.
 */
void Config::readJsonFile(const std::string& input)
{
	// The reading of the input file is based on
	// http://insanecoding.blogspot.cz/2011/11/how-to-read-in-file-in-c.html
	std::ifstream jsonFile(input, std::ios::in | std::ios::binary);
	if (!jsonFile)
	{
		std::string msg = "Input file \"" + input + "\" can not be opened.";
		throw FileNotFoundException(msg);
	}

	// A config file is a regular file.
	//
	// Opening a directory succeeds on glibc; seeking to its end then reports a
	// size that is not one -- -1 on some libstdc++ builds, LLONG_MAX on the
	// one here -- and `jsonContent.resize(jsonFile.tellg())` turned that into
	// std::length_error or std::bad_alloc, thrown out of a function whose
	// contract is FileNotFoundException or ParseException. Every caller
	// handles it on exactly those terms: fileinfo passes `-c`'s argument with
	// no is_regular_file check of its own and catches only those two, so
	// `fileinfo -c <directory>` aborted.
	std::error_code ec;
	if (!fs::is_regular_file(input, ec) || ec)
	{
		std::string msg = "Input file \"" + input + "\" is not a regular file.";
		throw FileNotFoundException(msg);
	}

	std::string jsonContent;
	jsonFile.seekg(0, std::ios::end);
	// ...and the size it reports still has to be one a string can hold, for a
	// file that changes underneath us or a device that opens as regular.
	const std::streamoff size = jsonFile.tellg();
	if (!jsonFile || size < 0 || static_cast<uint64_t>(size) > kMaxConfigBytes)
	{
		std::string msg = "Input file \"" + input + "\" can not be read.";
		throw FileNotFoundException(msg);
	}
	jsonContent.resize(static_cast<std::size_t>(size));
	jsonFile.seekg(0, std::ios::beg);
	if (!jsonContent.empty()) jsonFile.read(&jsonContent[0], jsonContent.size());
	jsonFile.close();

	readJsonString(jsonContent);
}

/**
 * Generates JSON configuration file.
 * @return Path to generated JSON file.
 */
std::string Config::generateJsonFile() const
{
	std::string out;
	if (!parameters.getOutputConfigFile().empty())
		out = parameters.getOutputConfigFile();
	return generateJsonFile( out );
}

/**
 * Generates JSON configuration file.
 * @param outputFilePath Path to output JSON file. If not set, use 'inputName'.
 * @return Path to generated JSON file.
 */
std::string Config::generateJsonFile(const std::string& outputFilePath) const
{
	std::string jsonName = outputFilePath.empty()
			? parameters.getInputFile() + ".json"
			: outputFilePath;

	std::ofstream jsonFile( jsonName.c_str() );
	jsonFile << generateJsonString();

	return jsonName;
}

/**
 * Generates string containing JSON representation of configuration.
 * @return JSON string.
 */
std::string Config::generateJsonString() const
{
	rapidjson::StringBuffer sb;
	rapidjson::PrettyWriter<rapidjson::StringBuffer, rapidjson::UTF8<>> writer(sb);

	writer.StartObject();

	serdes::serializeString(writer, JSON_date, retdec::utils::getCurrentDate());
	serdes::serializeString(writer, JSON_time, retdec::utils::getCurrentTime());

	writer.String(JSON_parameters);
	parameters.serialize(writer);

	serdes::serialize(writer, JSON_architecture, architecture);
	serdes::serialize(writer, JSON_fileType, fileType);
	serdes::serialize(writer, JSON_fileFormat, fileFormat);
	serdes::serializeContainer(writer, JSON_tools, tools);
	serdes::serializeContainer(writer, JSON_languages, languages);
	serdes::serializeContainer(writer, JSON_functions, functions);
	serdes::serializeContainer(writer, JSON_globals, globals);
	serdes::serializeContainer(writer, JSON_registers, registers);
	serdes::serializeContainer(writer, JSON_structures, structures);
	serdes::serializeContainer(writer, JSON_vtables, vtables);
	serdes::serializeContainer(writer, JSON_classes, classes);
	serdes::serializeContainer(writer, JSON_patterns, patterns);

	writer.EndObject();

	return sb.GetString();
}

/**
 * Reads string containing JSON representation of configuration.
 * If file can not be parsed, an instance of @c ParseException is thrown.
 * @param json JSON string.
 */
void Config::readJsonString(const std::string& json)
{
	rapidjson::Document root;
	rapidjson::ParseResult ok = root.Parse(json);
	if (!ok)
	{
		std::string errMsg = "Failed to parse configuration!";

		errMsg = GetParseError_En(ok.Code());

		auto loc = retdec::utils::getLineAndColumnFromPosition(json, ok.Offset());
		throw ParseException(errMsg, loc.first, loc.second);
	}

	*this = Config();

	auto params = root.FindMember(JSON_parameters);
	if (params != root.MemberEnd())
	{
		parameters.deserialize(params->value);
	}

	serdes::deserialize(root, JSON_architecture, architecture);
	serdes::deserialize(root, JSON_fileType, fileType);
	serdes::deserialize(root, JSON_fileFormat, fileFormat);

	serdes::deserializeContainer(root, JSON_tools, tools);
	serdes::deserializeContainer(root, JSON_languages, languages);
	serdes::deserializeContainer(root, JSON_functions, functions);
	serdes::deserializeContainer(root, JSON_globals, globals);
	serdes::deserializeContainer(root, JSON_registers, registers);
	serdes::deserializeContainer(root, JSON_structures, structures);
	serdes::deserializeContainer(root, JSON_vtables, vtables);
	serdes::deserializeContainer(root, JSON_classes, classes);
	serdes::deserializeContainer(root, JSON_patterns, patterns);
}

} // namespace config
} // namespace retdec
