// wav_encoder.h - IGI WAV ADPCM encoder
#pragma once

#include <string>

namespace igi {

/// @brief Encode standard PCM WAV to IGI IMA ADPCM format
/// @param input_path Path to input PCM WAV file
/// @param output_path Path to output IGI WAV file
/// @return 0 on success, 1 on error
int encode_adpcm(const std::string& input_path, const std::string& output_path);

} // namespace igi
