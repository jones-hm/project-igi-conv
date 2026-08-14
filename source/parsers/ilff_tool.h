// ilff_tool.h - IGI ILFF container parser/extractor
#pragma once

#include <string>

namespace igi {

/// @brief List all chunks in an ILFF container file
int list_ilff(const std::string& input_path);

/// @brief Extract a specific chunk from an ILFF container
/// @param input_path Path to ILFF file
/// @param fourcc FourCC code of chunk to extract (e.g., "XTRV", "DNER")
/// @param output_path Path to write extracted data
int extract_ilff_chunk(const std::string& input_path, const std::string& fourcc, const std::string& output_path);

} // namespace igi
