// qas_parser.h - IGI QAS AI script decompiler
#pragma once

#include <string>

namespace igi {

/// @brief Decompile binary QAS AI script to human-readable text
int decompile_qas(const std::string& input_path, const std::string& output_path);

} // namespace igi
