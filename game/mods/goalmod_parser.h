#pragma once

/*!
 * @file goalmod_parser.h
 * Parser for .goalmod mod definition files.
 */

#include <optional>
#include <string>

#include "game/mods/goalmod_types.h"

namespace mods {

/// Parse a .goalmod file from its text content. Returns nullopt on failure.
std::optional<ModDefinition> parse_goalmod(const std::string& content, std::string& error_out);

/// Parse a .goalmod file from disk.
std::optional<ModDefinition> parse_goalmod_file(const fs::path& path, std::string& error_out);

}  // namespace mods
