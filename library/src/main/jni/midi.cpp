////////////////////////////////////////////////////////////////////////////////
//
//  MidiDriver - An Android Midi Driver.
//
//  Copyright (C) 2013	Bill Farmer
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
//  Bill Farmer	 william j farmer [at] yahoo [dot] co [dot] uk.
//
///////////////////////////////////////////////////////////////////////////////

#include <jni.h>
#include <assert.h>

#include <atomic>

#include <android/log.h>

// for oboe native audio
#include <oboe/Oboe.h>

// for EAS midi
#define DLS_SYNTHESIZER
#include "eas.h"
#include "eas_reverb.h"

// for EAS_HWMemCpy
#include "eas_host.h"

#include "org_billthefarmer_mididriver_MidiDriver.h"
#include "midi.h"

#include <cstring>
#include <cstdlib>

#define LOG_TAG "MidiDriver"

#define LOG_D(tag, ...) __android_log_print(ANDROID_LOG_DEBUG, tag, __VA_ARGS__)
#define LOG_E(tag, ...) __android_log_print(ANDROID_LOG_ERROR, tag, __VA_ARGS__)
#define LOG_I(tag, ...) __android_log_print(ANDROID_LOG_INFO, tag, __VA_ARGS__)

// determines how many EAS buffers to fill a host buffer
#define NUM_BUFFERS 4

// typedef
typedef struct
{
    int len;
    const EAS_U8 *data;
} EAS_DLS_HANDLE;

// mutex
static std::atomic_flag mutex = ATOMIC_FLAG_INIT;

#define LOCK() while (mutex.test_and_set(std::memory_order_acquire));
#define UNLOCK() mutex.clear(std::memory_order_release);

// oboe stream
std::shared_ptr<oboe::AudioStream> oboeStream;

// EAS data
static EAS_DATA_HANDLE pEASData;
const S_EAS_LIB_CONFIG *pLibConfig;
static EAS_I32 bufferSize;
static EAS_HANDLE midiHandle;
static int isDLSLoaded;

// Functions
oboe::Result initOboe();
oboe::Result closeOboe();

// oboe callback
class OboeCallback: public oboe::AudioStreamDataCallback
{
public:
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *audioStream,
                                          void *audioData, int32_t numFrames)
    {
        EAS_RESULT result;
        EAS_I32 numGenerated;
        EAS_I32 count = 0;

        // We requested AudioFormat::I16. So if the stream opens
        // we know we got the I16 format.
        auto *outputData = static_cast<int16_t *>(audioData);

        while (count < bufferSize)
        {
            // lock
            LOCK();

            result = EAS_Render(pEASData, outputData + count,
                                pLibConfig->mixBufferSize, &numGenerated);
            // unlock
            UNLOCK();

            assert(result == EAS_SUCCESS);

            count += numGenerated * pLibConfig->numChannels;
        }

        return oboe::DataCallbackResult::Continue;
    }

    void onErrorAfterClose(oboe::AudioStream *audioStream, oboe::Result error)
    {
        if (error ==  oboe::Result::ErrorDisconnected)
            initOboe();
    }
};

// oboe callback
OboeCallback oboeCallback;

// build oboe
oboe::Result buildOboe()
{
    oboe::AudioStreamBuilder builder;

    builder.setDirection(oboe::Direction::Output);
    builder.setPerformanceMode(oboe::PerformanceMode::LowLatency);
    builder.setSampleRateConversionQuality(
        oboe::SampleRateConversionQuality::Medium);
    builder.setSharingMode(oboe::SharingMode::Exclusive);
    builder.setFormat(oboe::AudioFormat::I16);
    builder.setFramesPerCallback(bufferSize / pLibConfig->numChannels);
    builder.setChannelCount(pLibConfig->numChannels);
    builder.setSampleRate(pLibConfig->sampleRate);
    builder.setDataCallback(&oboeCallback);

    return builder.openStream(oboeStream);
}

oboe::Result initOboe()
{
    oboe::Result oboeResult;

    if ((oboeResult = buildOboe()) != oboe::Result::OK)
    {
        LOG_E(LOG_TAG, "Failed to create oboe stream. Error: %s",
              oboe::convertToText(oboeResult));

        return oboeResult;
    }

    if ((oboeResult = oboeStream->requestStart()) != oboe::Result::OK)
    {
        closeOboe();

        LOG_E(LOG_TAG, "Failed to start oboe stream. Error: %s",
              oboe::convertToText(oboeResult));

        return oboeResult;
    }

    return oboe::Result::OK;
}

// close oboe
oboe::Result closeOboe()
{
    if (oboeStream != NULL)
    {
        oboeStream->requestStop();
        return oboeStream->close();
    }

    return oboe::Result::ErrorNull;
}

// init EAS midi
EAS_RESULT initEAS()
{
    EAS_RESULT result;

    // get the library configuration
    pLibConfig = EAS_Config();
    if (pLibConfig == NULL || pLibConfig->libVersion != LIB_VERSION)
        return EAS_FAILURE;

    // calculate buffer size
    bufferSize = pLibConfig->mixBufferSize * pLibConfig->numChannels * NUM_BUFFERS;

    // init library
    if ((result = EAS_Init(&pEASData)) != EAS_SUCCESS)
        return result;

    // select reverb preset and enable
    EAS_SetParameter(pEASData, EAS_MODULE_REVERB, EAS_PARAM_REVERB_PRESET,
                     EAS_PARAM_REVERB_CHAMBER);
    EAS_SetParameter(pEASData, EAS_MODULE_REVERB, EAS_PARAM_REVERB_BYPASS,
                     EAS_FALSE);

    // open midi stream
    if ((result = EAS_OpenMIDIStream(pEASData, &midiHandle, NULL)) != EAS_SUCCESS)
        return result;

    isDLSLoaded = 0;

    return EAS_SUCCESS;
}

// shutdown EAS midi
void shutdownEAS()
{

    if (midiHandle != NULL)
    {
        EAS_CloseMIDIStream(pEASData, midiHandle);
        midiHandle = NULL;
    }

    if (pEASData != NULL)
    {
        EAS_Shutdown(pEASData);
        pEASData = NULL;
    }

    isDLSLoaded = 0;
}

// init mididriver
jboolean midi_init()
{
    EAS_RESULT result;
    oboe::Result oboeResult;

    if ((result = initEAS()) != EAS_SUCCESS)
    {
        shutdownEAS();

        LOG_E(LOG_TAG, "Init EAS failed: %ld", result);

        return JNI_FALSE;
    }

    // LOG_D(LOG_TAG, "Init EAS success, buffer: %ld", bufferSize);

    if ((oboeResult = initOboe()) != oboe::Result::OK)
    {
        shutdownEAS();

        return JNI_FALSE;
    }

    return JNI_TRUE;
}

jboolean
Java_org_billthefarmer_mididriver_MidiDriver_init(JNIEnv *env,
                                                  jobject obj)
{
    return midi_init();
}

// midi config
jintArray
Java_org_billthefarmer_mididriver_MidiDriver_config(JNIEnv *env,
                                                    jobject obj)
{
    jboolean isCopy;

    if (pLibConfig == NULL)
        return NULL;

    jintArray configArray = env->NewIntArray(4);

    jint *config = env->GetIntArrayElements(configArray, &isCopy);

    config[0] = pLibConfig->maxVoices;
    config[1] = pLibConfig->numChannels;
    config[2] = pLibConfig->sampleRate;
    config[3] = pLibConfig->mixBufferSize;

    env->ReleaseIntArrayElements(configArray, config, 0);

    return configArray;
}

// midi write
jboolean midi_write(EAS_U8 *bytes, jint length)
{
    EAS_RESULT result;

    if (pEASData == NULL || midiHandle == NULL)
        return JNI_FALSE;

    // lock
    LOCK();

    result = EAS_WriteMIDIStream(pEASData, midiHandle, bytes, length);

    // unlock
    UNLOCK();

    if (result != EAS_SUCCESS)
        return JNI_FALSE;

    return JNI_TRUE;
}

jboolean
Java_org_billthefarmer_mididriver_MidiDriver_write(JNIEnv *env,
                                                   jobject obj,
                                                   jbyteArray byteArray)
{
    jboolean result;
    jboolean isCopy;
    jint length;
    EAS_U8 *bytes;

    bytes = (EAS_U8 *) env->GetByteArrayElements(byteArray, &isCopy);
    length = env->GetArrayLength(byteArray);

    result = midi_write(bytes, length);

    env->ReleaseByteArrayElements(byteArray, (jbyte *) bytes, 0);

    return result;
}

// set EAS master volume
jboolean midi_setVolume(jint volume)
{
    EAS_RESULT result;

    if (pEASData == NULL || midiHandle == NULL)
        return JNI_FALSE;

    result = EAS_SetVolume(pEASData, NULL, (EAS_I32) volume);

    if (result != EAS_SUCCESS)
        return JNI_FALSE;

    return JNI_TRUE;
}

jboolean
Java_org_billthefarmer_mididriver_MidiDriver_setVolume(JNIEnv *env,
                                                       jobject obj,
                                                       jint volume)
{
    return midi_setVolume(volume);
}

// Set EAS reverb
jboolean midi_setReverb(jint preset)
{
    EAS_RESULT result;

    if (preset >= 0)
    {
        result = EAS_SetParameter(pEASData, EAS_MODULE_REVERB,
                                  EAS_PARAM_REVERB_PRESET, preset);
        if (result != EAS_SUCCESS)
        {
            LOG_E(LOG_TAG, "Set EAS reverb preset failed: %ld", result);
            return JNI_FALSE;
        }

        result = EAS_SetParameter(pEASData, EAS_MODULE_REVERB,
                                  EAS_PARAM_REVERB_BYPASS, EAS_FALSE);
        if (result != EAS_SUCCESS)
        {
            LOG_E(LOG_TAG, "Enable EAS reverb failed: %ld", result);
            return JNI_FALSE;
        }
    }

    else
    {
        result = EAS_SetParameter(pEASData, EAS_MODULE_REVERB,
                                  EAS_PARAM_REVERB_BYPASS, EAS_TRUE);
        if (result != EAS_SUCCESS)
        {
            LOG_E(LOG_TAG, "Disable EAS reverb failed: %ld", result);
            return JNI_FALSE;
        }
    }

    return JNI_TRUE;
}

jboolean
Java_org_billthefarmer_mididriver_MidiDriver_setReverb(JNIEnv *env,
                                                       jobject obj,
                                                       jint preset)
{
    return midi_setReverb(preset);
}

// shutdown EAS midi
jboolean midi_shutdown()
{
    closeOboe();
    shutdownEAS();

    return JNI_TRUE;
}

jboolean
Java_org_billthefarmer_mididriver_MidiDriver_shutdown(JNIEnv *env,
                                                      jobject obj)
{
    return midi_shutdown();
}

static int memDLS_readAt(void *handle, void *buf, int offset, int size)
{
    const EAS_U8 *data;
    EAS_DLS_HANDLE *pHandle;

    pHandle = (EAS_DLS_HANDLE *) handle;
    data = pHandle->data;
    EAS_HWMemCpy(buf, data + offset, size);

    return size;
}

static int memDLS_size(void *handle)
{
    EAS_DLS_HANDLE *pHandle;

    pHandle = (EAS_DLS_HANDLE *) handle;
    return pHandle->len;
}

jboolean midi_loadDLS(const EAS_U8 *dlsData, jint length)
{
    EAS_FILE file;
    EAS_RESULT result;
    EAS_DLS_HANDLE handle = { length, dlsData };

    file.handle = (void *) &handle;
    file.readAt = memDLS_readAt;
    file.size = memDLS_size;

    result = EAS_LoadDLSCollection(pEASData, midiHandle, &file);
    if (result != EAS_SUCCESS)
        return JNI_FALSE;

    isDLSLoaded = 1;
    return JNI_TRUE;
}

// WAV file writer utility
struct WavHeader {
    char riffChunk[4];      // "RIFF"
    uint32_t riffSize;      // file size - 8
    char waveChunk[4];      // "WAVE"
    char fmtChunk[4];       // "fmt "
    uint32_t fmtSize;       // format chunk size (16)
    uint16_t audioFormat;   // 1 = PCM
    uint16_t numChannels;   // 2 for stereo
    uint32_t sampleRate;    // 44100, 48000, etc.
    uint32_t byteRate;      // sampleRate * numChannels * bytesPerSample
    uint16_t blockAlign;    // numChannels * bytesPerSample
    uint16_t bitsPerSample; // 16
    char dataChunk[4];       // "data"
    uint32_t dataSize;      // audio data size in bytes
};

static void writeWavHeader(FILE *file, uint32_t sampleRate, 
                          uint32_t numSamples, uint16_t numChannels)
{
    WavHeader header;
    uint32_t byteRate = sampleRate * numChannels * 2;
    uint32_t dataSize = numSamples * numChannels * 2;

    memcpy(header.riffChunk, "RIFF", 4);
    header.riffSize = 36 + dataSize;
    memcpy(header.waveChunk, "WAVE", 4);
    memcpy(header.fmtChunk, "fmt ", 4);
    header.fmtSize = 16;
    header.audioFormat = 1;      // PCM
    header.numChannels = numChannels;
    header.sampleRate = sampleRate;
    header.byteRate = byteRate;
    header.blockAlign = numChannels * 2;
    header.bitsPerSample = 16;
    memcpy(header.dataChunk, "data", 4);
    header.dataSize = dataSize;

    fwrite(&header, sizeof(WavHeader), 1, file);
}

jboolean midi_exportToWav(const char *outputPath, uint32_t durationMs)
{
    EAS_RESULT result;
    EAS_I32 numGenerated;
    FILE *file;
    int16_t *buffer;
    int16_t *outputBuffer;
    uint32_t totalSamples;
    uint32_t samplesWritten = 0;

    if (pEASData == NULL || midiHandle == NULL)
        return JNI_FALSE;

    file = fopen(outputPath, "wb");
    if (file == NULL)
    {
        LOG_E(LOG_TAG, "Failed to open output file: %s", outputPath);
        return JNI_FALSE;
    }

    // Allocate buffer for audio data
    buffer = (int16_t *) malloc(bufferSize * sizeof(int16_t));
    if (buffer == NULL)
    {
        LOG_E(LOG_TAG, "Failed to allocate buffer");
        fclose(file);
        return JNI_FALSE;
    }

    // Calculate total samples for duration
    totalSamples = (pLibConfig->sampleRate * durationMs) / 1000;
    
    // Write WAV header (will update file size later if needed)
    writeWavHeader(file, pLibConfig->sampleRate, totalSamples, 
                   pLibConfig->numChannels);

    // Render audio and write to file
    outputBuffer = (int16_t *) malloc(bufferSize * sizeof(int16_t));
    if (outputBuffer == NULL)
    {
        LOG_E(LOG_TAG, "Failed to allocate output buffer");
        free(buffer);
        fclose(file);
        return JNI_FALSE;
    }

    while (samplesWritten < totalSamples)
    {
        LOCK();
        result = EAS_Render(pEASData, outputBuffer, 
                           pLibConfig->mixBufferSize, &numGenerated);
        UNLOCK();

        if (result != EAS_SUCCESS)
        {
            LOG_E(LOG_TAG, "EAS_Render failed: %ld", result);
            break;
        }

        if (numGenerated > 0)
        {
            uint32_t samples = numGenerated * pLibConfig->numChannels;
            uint32_t toWrite = (samplesWritten + samples > totalSamples) ?
                             (totalSamples - samplesWritten) : samples;
            
            fwrite(outputBuffer, sizeof(int16_t), toWrite, file);
            samplesWritten += toWrite;
        }

        if (numGenerated == 0)
            break;
    }

    free(outputBuffer);
    free(buffer);
    fclose(file);

    LOG_I(LOG_TAG, "WAV export complete: %s (%u samples)", 
          outputPath, samplesWritten);

    return JNI_TRUE;
}

jboolean
Java_org_billthefarmer_mididriver_MidiDriver_loadDLS(JNIEnv *env,
                                                     jobject obj,
                                                     jbyteArray byteArray)
{
    jint length;
    EAS_U8 *bytes;
    jboolean isCopy;
    jboolean result;

    if (isDLSLoaded != 0)
        return JNI_FALSE;

    bytes = (EAS_U8 *) env->GetByteArrayElements(byteArray, &isCopy);
    length = env->GetArrayLength(byteArray);

    result = midi_loadDLS(bytes, length);

    env->ReleaseByteArrayElements(byteArray, (jbyte *) bytes, 0);

    return result;
}
