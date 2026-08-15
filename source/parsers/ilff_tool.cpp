// ilff_tool.cpp - IGI ILFF container parser/extractor
// Direct CLI options for InnerLoop File Format container structure

#include "ilff_tool.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

namespace igi {

#pragma pack(push, 1)
struct IlffHeader {
    char magic[4];          // "ILFF"
    uint32_t file_size;
    uint32_t alignment;     // always 4
    uint32_t skip;          // always 0 in outer header
    char format_id[4];      // "HSEM" for MEF, "IRES" for RES
};

struct ChunkHeader {
    char fourcc[4];         // FourCC chunk identifier
    uint32_t data_size;
    uint32_t alignment;
    uint32_t skip;          // offset to next chunk (0 = last)
};
#pragma pack(pop)

static void print_chunk(const char* prefix, const ChunkHeader* chunk, size_t offset) {
    printf("%s Chunk '%.4s' at 0x%04zx: size=%u, align=%u, skip=%u\n",
           prefix, chunk->fourcc, offset, chunk->data_size, chunk->alignment, chunk->skip);
}

int list_ilff(const std::string& input_path) {
    std::ifstream infile(input_path, std::ios::binary);
    if (!infile) {
        fprintf(stderr, "Error: Cannot open input file %s\n", input_path.c_str());
        return 1;
    }

    infile.seekg(0, std::ios::end);
    size_t file_size = infile.tellg();
    infile.seekg(0, std::ios::beg);

    if (file_size < sizeof(IlffHeader)) {
        fprintf(stderr, "Error: File too small for ILFF header\n");
        return 1;
    }

    IlffHeader header;
    infile.read(reinterpret_cast<char*>(&header), sizeof(header));

    if (memcmp(header.magic, "ILFF", 4) != 0) {
        fprintf(stderr, "Error: Not an ILFF file (magic: %.4s)\n", header.magic);
        return 1;
    }

    printf("ILFF Container: %s\n", input_path.c_str());
    printf("  Format: %.4s\n", header.format_id);
    printf("  File size: %u bytes\n", header.file_size);
    printf("  Alignment: %u\n", header.alignment);
    printf("\nChunks:\n");

    size_t offset = sizeof(IlffHeader);
    int chunk_count = 0;

    while (offset + sizeof(ChunkHeader) <= file_size) {
        ChunkHeader chunk;
        infile.read(reinterpret_cast<char*>(&chunk), sizeof(chunk));
        
        print_chunk("  ", &chunk, offset);
        chunk_count++;

        if (chunk.skip == 0) break;
        offset += chunk.skip;
        infile.seekg(offset);
    }

    printf("\nTotal chunks: %d\n", chunk_count);
    infile.close();
    return 0;
}

int extract_ilff_chunk(const std::string& input_path, const std::string& fourcc, const std::string& output_path) {
    // Validate FourCC length
    if (fourcc.size() != 4) {
        fprintf(stderr, "Error: FourCC must be exactly 4 characters\n");
        return 1;
    }
    
    std::ifstream infile(input_path, std::ios::binary);
    if (!infile) {
        fprintf(stderr, "Error: Cannot open input file %s\n", input_path.c_str());
        return 1;
    }

    infile.seekg(0, std::ios::end);
    std::streamsize file_size = infile.tellg();
    infile.seekg(0, std::ios::beg);
    
    // Validate file size
    if (file_size < static_cast<std::streamsize>(sizeof(IlffHeader))) {
        fprintf(stderr, "Error: File too small for ILFF header\n");
        return 1;
    }

    IlffHeader header{};
    if (!infile.read(reinterpret_cast<char*>(&header), sizeof(header))) {
        fprintf(stderr, "Error: Failed to read ILFF header\n");
        return 1;
    }

    if (memcmp(header.magic, "ILFF", 4) != 0) {
        fprintf(stderr, "Error: Not an ILFF file\n");
        return 1;
    }

    size_t offset = sizeof(IlffHeader);

    while (offset + sizeof(ChunkHeader) <= static_cast<size_t>(file_size)) {
        ChunkHeader chunk{};
        if (!infile.read(reinterpret_cast<char*>(&chunk), sizeof(chunk))) {
            break;
        }
        
        // Validate chunk bounds
        if (chunk.data_size > static_cast<uint32_t>(file_size)) {
            fprintf(stderr, "Error: Chunk data_size exceeds file size\n");
            return 1;
        }
        
        if (memcmp(chunk.fourcc, fourcc.c_str(), 4) == 0) {
            // Found the chunk - extract data
            std::vector<uint8_t> data(chunk.data_size);
            if (!infile.read(reinterpret_cast<char*>(data.data()), chunk.data_size)) {
                fprintf(stderr, "Error: Failed to read chunk data\n");
                return 1;
            }
            infile.close();

            // Write to temporary file first (atomic write)
            std::string tmp_path = output_path + ".tmp";
            std::ofstream outfile(tmp_path, std::ios::binary);
            if (!outfile) {
                fprintf(stderr, "Error: Cannot create output file %s\n", output_path.c_str());
                return 1;
            }
            
            if (!outfile.write(reinterpret_cast<char*>(data.data()), data.size())) {
                fprintf(stderr, "Error: Failed to write output\n");
                outfile.close();
                std::remove(tmp_path.c_str());
                return 1;
            }
            
            if (!outfile.good()) {
                fprintf(stderr, "Error: Output stream in error state\n");
                outfile.close();
                std::remove(tmp_path.c_str());
                return 1;
            }
            
            outfile.close();
            
            // Rename temporary file to final output
            std::rename(tmp_path.c_str(), output_path.c_str());

            printf("Extracted chunk '%.4s' (%u bytes) -> %s\n",
                   chunk.fourcc, chunk.data_size, output_path.c_str());
            return 0;
        }

        if (chunk.skip == 0) break;
        
        // Validate skip offset
        if (chunk.skip < sizeof(ChunkHeader) || 
            offset + chunk.skip > static_cast<size_t>(file_size)) {
            fprintf(stderr, "Error: Invalid chunk skip offset\n");
            return 1;
        }
        
        offset += chunk.skip;
        infile.seekg(offset);
    }

    fprintf(stderr, "Error: Chunk '%.4s' not found\n", fourcc.c_str());
    infile.close();
    return 1;
}

        if (chunk.skip == 0) break;
        offset += chunk.skip;
        infile.seekg(offset);
    }

    fprintf(stderr, "Error: Chunk '%.4s' not found\n", fourcc.c_str());
    infile.close();
    return 1;
}

} // namespace igi
