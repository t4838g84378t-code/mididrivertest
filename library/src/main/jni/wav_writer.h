#ifndef WAV_WRITER_H
#define WAV_WRITER_H

#include <stdio.h>
#include <stdint.h>

struct WavHeader {
    char riff[4];
    uint32_t chunkSize;
    char wave[4];
    char fmt[4];
    uint32_t fmtSize;
    uint16_t audioFormat;
    uint16_t numChannels;
    uint32_t sampleRate;
    uint32_t byteRate;
    uint16_t blockAlign;
    uint16_t bitsPerSample;
    char data[4];
    uint32_t dataSize;
};

inline void writeWavHeader(FILE* fp, uint32_t dataBytes) {
    WavHeader header = {
        {'R', 'I', 'F', 'F'},
        36 + dataBytes,
        {'W', 'A', 'V', 'E'},
        {'f', 'm', 't', ' '},
        16,
        1,
        2,
        44100,
        44100 * 2 * 2,
        4,
        16,
        {'d', 'a', 't', 'a'},
        dataBytes
    };
    fseek(fp, 0, SEEK_SET);
    fwrite(&header, sizeof(WavHeader), 1, fp);
    fflush(fp);
}

#endif // WAV_WRITER_H