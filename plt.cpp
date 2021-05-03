#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <filesystem>

#include <cstdint>
#include <cstring>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "include/stb/stb_image.h"
#include "include/stb/stb_image_write.h"

namespace fs = std::filesystem;

#pragma pack(push, 1)
struct PltHeader
{
    int magic;
    uint16_t file_count;
    uint32_t x, y;
    uint32_t w, h;
};

struct PltBaseEntry
{
    uint32_t x, y;
    uint32_t w, h, c;
};

struct PltSubEntry
{
    uint8_t bands;
    uint32_t length;
};
#pragma pack(pop)

static std::vector<uint8_t> keydb;

void decompress_rle(
    std::vector<uint8_t>& input, const size_t size_orig, const size_t bands)
{
    std::vector<uint8_t> output(size_orig);
    const uint8_t* const in_start = (uint8_t*)input.data();
    const uint8_t* const in_end = in_start + input.size();
    uint8_t* const out_start = (uint8_t*)output.data();
    uint8_t* const out_end = out_start + size_orig;
    const uint8_t* pin = in_start;
    uint8_t* pout;

    for (size_t i = 0; i < bands; i++)
    {
        pout = out_start + i;
        const uint8_t init_b = *pin++;
        uint8_t last_b = init_b;
        if (pout >= out_end)
            throw std::runtime_error("Unexpected EOF!");
        *pout = init_b;
        pout += bands;

        while (pout < out_end)
        {
            auto b = *pin++;
            *pout = b;
            pout += bands;
            if (last_b == b)
            {
                uint16_t repetitions = *pin++;
                if (repetitions >= 0x80)
                {
                    repetitions &= 0x7F;
                    repetitions <<= 8;
                    repetitions |= *pin++;
                    repetitions += 0x80;
                }
                while (repetitions-- && pout < out_end)
                {
                    *pout = b;
                    pout += bands;
                }

                if (pout < out_end)
                {
                    b = *pin++;
                    *pout = b;
                    pout += bands;
                }
            }
            last_b = b;
        }
    }
    input = std::move(output);
}

void init_keydb(const std::string& filename)
{
    std::ifstream fin(filename, std::ios::binary | std::ios::ate);
    if (!fin.is_open())
    {
        std::cout << "Failed to load keydb\n";
        exit(1);
    }
    size_t s = fin.tellg();
    fin.seekg(0);
    keydb.resize(s);
    fin.read((char*)keydb.data(), s);
}

void decrypt(std::vector<uint8_t>& data)
{
    size_t keylen = keydb.size();
    size_t count = data.size() / keylen;
    uint8_t* pos = data.data();
    const uint8_t* key;
    for (size_t i = 0; i < count; i++)
    {
        key = keydb.data();
        for (size_t j = 0; j < keylen; j++)
            *pos++ ^= *key++;
    }
    size_t rest = data.size() % keylen;
    if (rest > 0)
    {
        key = keydb.data();
        for (size_t j = 0; j < rest; j++)
            *pos++ ^= *key++;
    }
}


std::vector<uint8_t> flip3(const std::vector<uint8_t>& data, int w, int h)
{
    const int stride = w * 3;
    std::vector<uint8_t> buf(data.size());
    const uint8_t *in;
    uint8_t *out;
    for (int row = 0; row < h; row++)
    {
        in = data.data() + (h - row - 1) * stride;
        out = buf.data() + row * stride;
        for (int p = 0; p < w; p++)
        {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out += 3;
            in += 3;
        }
    }
    return buf;
}
std::vector<uint8_t> flip4(const std::vector<uint8_t>& data, int w, int h)
{
    const int stride = w * 4;
    std::vector<uint8_t> buf(data.size());
    const uint8_t *in;
    uint8_t *out;
    for (int row = 0; row < h; row++)
    {
        in = data.data() + (h - row - 1) * stride;
        out = buf.data() + row * stride;
        for (int p = 0; p < w; p++)
        {
            out[0] = in[3];
            out[1] = in[2];
            out[2] = in[1];
            out[3] = in[0];
            out += 4;
            in += 4;
        }
    }
    return buf;
}

void save_image(const std::string& filename, const std::vector<uint8_t>& data, int w, int h, int c)
{
    std::vector<uint8_t> res;
    if (c == 3)
        res = flip3(data, w, h);
    else if (c == 4)
        res = flip4(data, w, h);
    else
    {
        std::cerr << "Unsupported pixel format: " << (c * 8) << " bpp\n";
        exit(1);
    }
    stbi_write_png(filename.c_str(), w, h, c, res.data(), w * c);
}

int main(int argc, const char** argv)
{
    if (argc != 2)
    {
        std::cout << "Usage: plt.exe <input>\n";
        return 1;
    }
    fs::path fname(argv[1]);
    fs::path outpath(fname);
    outpath.replace_extension();
    outpath += "_";

    std::ifstream fin(fname, std::ios::binary);
    if (!fin.is_open())
    {
        std::cerr << "Could not open file " << fname << std::endl;
        return 1;
    }

    init_keydb("keys.bin");

    PltHeader hdr;
    PltBaseEntry base;
    PltSubEntry entry;
    std::vector<uint8_t> last;
    std::vector<uint8_t> image;

    fin.read((char*)&hdr, sizeof(hdr));
    fin.read((char*)&base, sizeof(base));

    const size_t image_size = base.w * base.h * base.c;
    image.resize(image_size);
    fin.read((char*)image.data(), image.size());

    decrypt(image);
    save_image(outpath.string() + "00.png", image, base.w, base.h, base.c);
    last = std::move(image);

    for (uint32_t i = 1; i < hdr.file_count; i++)
    {
        fin.read((char*)&entry, sizeof(entry));
        image.resize(entry.length);
        fin.read((char*)image.data(), entry.length);
        
        decompress_rle(image, base.w * base.h * base.c, entry.bands);

        // add diff image to base image
        for (uint8_t *pi = image.data(),
                     *pl = last.data(),
                     *const iend = pi + image.size();
                pi < iend; )
            *pi++ += *pl++;

        std::string fn = outpath.string();
        if (i < 10)
            fn += '0';
        fn += std::to_string(i) + ".png";
        save_image(fn, image, base.w, base.h, base.c);
        
        last = std::move(image);
    }

    return 0;
}