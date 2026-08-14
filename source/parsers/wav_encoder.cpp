// wav_encoder.cpp - IGI WAV ADPCM encoder (PCM -> IGI ADPCM)
// Based on IMA ADPCM 4-bit encoding (predictor=0, step_index=0)

#include "wav_encoder.h"
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <vector>
#include <string>
#include <fstream>

// IMA ADPCM step table
static const int step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32777, 36055,
    39660, 43627, 47990, 52789, 58068, 63875, 70262, 77288, 85018, 93520,
    102872, 113160, 124476, 136924, 149616, 164578, 181036, 199139, 219053, 240958
};

// IMA ADPCM index adjustment table
static const int index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8
};

namespace igi {

struct WavHeader {
    char riff[4] = {'R', 'I', 'F', 'F'};
    uint32_t file_size;
    char wave[4] = {'W', 'A', 'V', 'E'};
    char fmt[4] = {'f', 'm', 't', ' '};
    uint32_t fmt_size = 16;
    uint16_t format_tag = 0x0011; // IMA ADPCM
    uint16_t channels = 1;
    uint32_t samples_per_sec = 22050;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample = 4;
    char data[4] = {'d', 'a', 't', 'a'};
    uint32_t data_size;
};

int encode_adpcm(const std::string& input_path, const std::string& output_path) {
    // Read input PCM WAV
    std::ifstream infile(input_path, std::ios::binary);
    if (!infile) {
        fprintf(stderr, "Error: Cannot open input file %s\n", input_path.c_str());
        return 1;
    }

    // Read PCM data (simplified - assumes PCM WAV input)
    infile.seekg(0, std::ios::end);
    size_t pcm_size = infile.tellg();
    infile.seekg(0, std::ios::beg);
    
    std::vector<uint8_t> pcm_data(pcm_size);
    infile.read(reinterpret_cast<char*>(pcm_data.data()), pcm_size);
    infile.close();

    // Encode to IMA ADPCM
    int predictor = 0;
    int step_index = 0;
    
    size_t adpcm_size = pcm_size / 2;
    std::vector<uint8_t> adpcm_data(adpcm_size);
    
    for (size_t i = 0, j = 0; i < pcm_size && j < adpcm_size; i += 2, j++) {
        int sample = (int16_t)(pcm_data[i] | (pcm_data[i+1] << 8));
        int step = step_table[step_index];
        int diff = sample - predictor;
        
        uint8_t nibble = 0;
        if (diff < 0) {
            nibble = 8;
            diff = -diff;
        }
        
        if (diff >= step) { nibble |= 4; diff -= step; }
        if (diff >= (step >> 1)) { nibble |= 2; diff -= (step >> 1); }
        if (diff >= (step >> 2)) { nibble |= 1; }
        
        // Update predictor
        int diff_calc = step >> 3;
        if (nibble & 4) diff_calc += step;
        if (nibble & 2) diff_calc += (step >> 1);
        if (nibble & 1) diff_calc += (step >> 2);
        
        if (nibble & 8) predictor -= diff_calc;
        else predictor += diff_calc;
        
        // Clamp
        if (predictor > 32767) predictor = 32767;
        if (predictor < -32768) predictor = -32768;
        
        // Update step index
        step_index += index_table[nibble];
        if (step_index < 0) step_index = 0;
        if (step_index > 88) step_index = 88;
        
        // Pack nibble (stereo interleaved if needed)
        if (j % 2 == 0) adpcm_data[j/2] = (nibble << 4);
        else adpcm_data[j/2] |= nibble;
    }

    // Write IGI WAV output
    std::ofstream outfile(output_path, std::ios::binary);
    if (!outfile) {
        fprintf(stderr, "Error: Cannot create output file %s\n", output_path.c_str());
        return 1;
    }

    // Build header
    WavHeader header;
    header.channels = 1;
    header.samples_per_sec = 22050;
    header.bits_per_sample = 4;
    header.block_align = 0x200;
    header.avg_bytes_per_sec = header.samples_per_sec * header.block_align / header.bits_per_sample;
    header.data_size = (uint32_t)adpcm_data.size();
    header.file_size = header.data_size + sizeof(WavHeader) - 8;

    outfile.write(reinterpret_cast<char*>(&header), sizeof(header));
    outfile.write(reinterpret_cast<char*>(adpcm_data.data()), adpcm_data.size());
    outfile.close();

    printf("Encoded %s -> %s (%zu bytes PCM -> %zu bytes ADPCM)\n",
           input_path.c_str(), output_path.c_str(), pcm_size, adpcm_data.size());
    return 0;
}

} // namespace igi
