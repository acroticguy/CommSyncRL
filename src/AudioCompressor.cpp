#include "SyncComms/AudioCompressor.h"
#include "miniaudio.h"

#include <vorbis/vorbisenc.h>
#include <ogg/ogg.h>

#include <cstring>
#include <cstdlib>
#include <ctime>

namespace SyncComms {

std::vector<uint8_t> AudioCompressor::CompressWavToOgg(const std::string& wavPath,
                                                        int sampleRate, int channels,
                                                        float quality) {
    std::vector<uint8_t> output;

    // Open WAV with miniaudio decoder
    ma_decoder decoder;
    ma_decoder_config decoderConfig = ma_decoder_config_init(ma_format_f32,
        static_cast<ma_uint32>(channels),
        static_cast<ma_uint32>(sampleRate));

    if (ma_decoder_init_file(wavPath.c_str(), &decoderConfig, &decoder) != MA_SUCCESS) {
        return output;
    }

    // Initialize vorbis encoder
    vorbis_info vi;
    vorbis_info_init(&vi);

    if (vorbis_encode_init_vbr(&vi, channels, sampleRate, quality) != 0) {
        vorbis_info_clear(&vi);
        ma_decoder_uninit(&decoder);
        return output;
    }

    vorbis_comment vc;
    vorbis_comment_init(&vc);
    vorbis_comment_add_tag(&vc, "ENCODER", "SyncComms");

    vorbis_dsp_state vd;
    vorbis_block vb;
    vorbis_analysis_init(&vd, &vi);
    vorbis_block_init(&vd, &vb);

    ogg_stream_state os;
    srand(static_cast<unsigned>(time(nullptr)));
    ogg_stream_init(&os, rand());

    // Write vorbis headers
    ogg_packet header, header_comm, header_code;
    vorbis_analysis_headerout(&vd, &vc, &header, &header_comm, &header_code);
    ogg_stream_packetin(&os, &header);
    ogg_stream_packetin(&os, &header_comm);
    ogg_stream_packetin(&os, &header_code);

    // Flush headers
    ogg_page og;
    while (ogg_stream_flush(&os, &og)) {
        output.insert(output.end(), og.header, og.header + og.header_len);
        output.insert(output.end(), og.body, og.body + og.body_len);
    }

    // Encode audio in chunks
    const int READ_SIZE = 4096;
    std::vector<float> readBuf(READ_SIZE * channels);
    bool eos = false;

    while (!eos) {
        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(&decoder, readBuf.data(), READ_SIZE, &framesRead);

        if (framesRead == 0) {
            // End of input — signal end to encoder
            vorbis_analysis_wrote(&vd, 0);
        } else {
            // Expose buffer to vorbis and copy interleaved samples
            float** buffer = vorbis_analysis_buffer(&vd, static_cast<int>(framesRead));
            for (ma_uint64 i = 0; i < framesRead; i++) {
                for (int ch = 0; ch < channels; ch++) {
                    buffer[ch][i] = readBuf[i * channels + ch];
                }
            }
            vorbis_analysis_wrote(&vd, static_cast<int>(framesRead));
        }

        // Pull encoded packets
        while (vorbis_analysis_blockout(&vd, &vb) == 1) {
            vorbis_analysis(&vb, nullptr);
            vorbis_bitrate_addblock(&vb);

            ogg_packet op;
            while (vorbis_bitrate_flushpacket(&vd, &op)) {
                ogg_stream_packetin(&os, &op);

                while (!eos) {
                    if (!ogg_stream_pageout(&os, &og)) break;
                    output.insert(output.end(), og.header, og.header + og.header_len);
                    output.insert(output.end(), og.body, og.body + og.body_len);
                    if (ogg_page_eos(&og)) eos = true;
                }
            }
        }
    }

    // Cleanup
    ogg_stream_clear(&os);
    vorbis_block_clear(&vb);
    vorbis_dsp_clear(&vd);
    vorbis_comment_clear(&vc);
    vorbis_info_clear(&vi);
    ma_decoder_uninit(&decoder);

    return output;
}

// Base64 encoding table
static const char B64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string AudioCompressor::Base64Encode(const std::vector<uint8_t>& data) {
    std::string result;
    result.reserve(((data.size() + 2) / 3) * 4);

    size_t i = 0;
    while (i < data.size()) {
        uint32_t a = i < data.size() ? data[i++] : 0;
        uint32_t b = i < data.size() ? data[i++] : 0;
        uint32_t c = i < data.size() ? data[i++] : 0;
        uint32_t triple = (a << 16) | (b << 8) | c;

        result += B64_TABLE[(triple >> 18) & 0x3F];
        result += B64_TABLE[(triple >> 12) & 0x3F];
        result += (i > data.size() + 1) ? '=' : B64_TABLE[(triple >> 6) & 0x3F];
        result += (i > data.size()) ? '=' : B64_TABLE[triple & 0x3F];
    }

    return result;
}

static const uint8_t B64_DECODE_TABLE[256] = {
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,62,64,64,64,63,
    52,53,54,55,56,57,58,59,60,61,64,64,64,64,64,64,
    64, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
    15,16,17,18,19,20,21,22,23,24,25,64,64,64,64,64,
    64,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
    41,42,43,44,45,46,47,48,49,50,51,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
    64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,64,
};

std::vector<uint8_t> AudioCompressor::Base64Decode(const std::string& encoded) {
    std::vector<uint8_t> result;
    result.reserve(encoded.size() * 3 / 4);

    uint32_t accum = 0;
    int bits = 0;

    for (char c : encoded) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        uint8_t val = B64_DECODE_TABLE[static_cast<uint8_t>(c)];
        if (val == 64) continue;

        accum = (accum << 6) | val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            result.push_back(static_cast<uint8_t>((accum >> bits) & 0xFF));
        }
    }

    return result;
}

} // namespace SyncComms
