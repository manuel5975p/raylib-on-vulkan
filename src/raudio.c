/**********************************************************************************************
*
*   raudio - raylib-on-vulkan audio module (raylib-compatible)
*
*   DESCRIPTION:
*       Audio device management, wave/sound loading, music streaming and raw audio streams,
*       implemented on top of miniaudio low-level device API. Mixing is done internally by
*       this module (NOT by miniaudio's high-level engine): every playable object owns an
*       AudioBuffer, all live buffers are kept on a linked list, and the device data callback
*       walks that list mixing each buffer (with its own volume/pitch/pan) into the output.
*
*   ARCHITECTURE:
*       - ma_device (f32, stereo) with data callback -> OnSendAudioDataToDevice()
*       - AudioBuffer: raw PCM in its own format + ma_data_converter to the mixing format
*       - AUDIO_BUFFER_USAGE_STATIC: whole sound preconverted, read linearly (Sound)
*       - AUDIO_BUFFER_USAGE_STREAM: double buffer (two sub-buffers) refilled by the main
*         thread (Music/AudioStream) or filled directly by an audio-thread callback
*       - ma_mutex protects the buffer list and the processor lists
*
*   FILE FORMATS (loading): WAV (dr_wav), OGG (stb_vorbis), MP3 (dr_mp3), FLAC (dr_flac),
*   QOA (qoa.h/qoaplay.c); music streaming additionally supports XM (jar_xm), MOD (jar_mod)
*   FILE FORMATS (exporting): WAV (dr_wav), QOA (qoa.h), RAW, and .h code export
*
*   MODULE OWNERSHIP (see CONVENTIONS.md): this file owns MINIAUDIO_IMPLEMENTATION,
*   DR_WAV/DR_MP3/DR_FLAC implementations, stb_vorbis.c, QOA_IMPLEMENTATION, qoaplay.c,
*   JAR_XM_IMPLEMENTATION and JAR_MOD_IMPLEMENTATION
*
**********************************************************************************************/

#include "raylib.h"                 // Module header: Wave, Sound, Music, AudioStream

#include "config.h"                 // Defines module configuration flags

#if defined(SUPPORT_MODULE_RAUDIO)

#include <stdlib.h>                 // Required for: malloc(), free()
#include <stdio.h>                  // Required for: FILE, fopen(), fclose(), fread(), sprintf()
#include <string.h>                 // Required for: strcmp(), strncpy(), memcpy(), memset()

//----------------------------------------------------------------------------------
// Defines and Macros
//----------------------------------------------------------------------------------
#ifndef RL_MALLOC
    #define RL_MALLOC(sz)       malloc(sz)
#endif
#ifndef RL_CALLOC
    #define RL_CALLOC(n,sz)     calloc(n,sz)
#endif
#ifndef RL_REALLOC
    #define RL_REALLOC(ptr,sz)  realloc(ptr,sz)
#endif
#ifndef RL_FREE
    #define RL_FREE(ptr)        free(ptr)
#endif

// Audio device output configuration
#ifndef AUDIO_DEVICE_FORMAT
    #define AUDIO_DEVICE_FORMAT             ma_format_f32   // Device output format (miniaudio: float-32bit)
#endif
#ifndef AUDIO_DEVICE_CHANNELS
    #define AUDIO_DEVICE_CHANNELS                       2   // Device output channels: stereo
#endif
#ifndef AUDIO_DEVICE_SAMPLE_RATE
    #define AUDIO_DEVICE_SAMPLE_RATE                    0   // Device sample rate (0: device default)
#endif
#ifndef AUDIO_DEVICE_PERIOD_SIZE_IN_FRAMES
    #define AUDIO_DEVICE_PERIOD_SIZE_IN_FRAMES          0   // Device period size (0: backend default, ~10ms)
#endif

#ifndef DEFAULT_AUDIO_BUFFER_SIZE_DIVISOR
    #define DEFAULT_AUDIO_BUFFER_SIZE_DIVISOR          30   // Streaming sub-buffer = sampleRate/30 (~33ms)
#endif

//----------------------------------------------------------------------------------
// Third-party libraries: implementations owned by this module
//----------------------------------------------------------------------------------
#define MA_MALLOC RL_MALLOC
#define MA_FREE RL_FREE
#define MA_REALLOC RL_REALLOC

// Disable everything we do not use: decoding/encoding is handled by dr_libs/stb_vorbis/qoa
// (this also avoids duplicated dr_wav/dr_flac/dr_mp3 symbols inside miniaudio)
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_WAV
#define MA_NO_FLAC
#define MA_NO_MP3
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MA_NO_GENERATION
#define MA_NO_JACK

// On Win32, windows.h (pulled in by miniaudio) declares symbols clashing with
// raylib.h (Rectangle, CloseWindow, ShowCursor, ...): exclude those API subsets
#if defined(_WIN32)
    #define NOGDICAPMASKS
    #define NOVIRTUALKEYCODES
    #define NOWINMESSAGES
    #define NOWINSTYLES
    #define NOSYSMETRICS
    #define NOMENUS
    #define NOICONS
    #define NOKEYSTATES
    #define NOSYSCOMMANDS
    #define NORASTEROPS
    #define NOSHOWWINDOW
    #define OEMRESOURCE
    #define NOATOM
    #define NOCLIPBOARD
    #define NOCOLOR
    #define NOCTLMGR
    #define NODRAWTEXT
    #define NOGDI
    #define NOKERNEL
    #define NOUSER
    #define NOMB
    #define NOMEMMGR
    #define NOMETAFILE
    #define NOMINMAX
    #define NOMSG
    #define NOOPENFILE
    #define NOSCROLL
    #define NOSERVICE
    #define NOSOUND
    #define NOTEXTMETRIC
    #define NOWH
    #define NOWINOFFSETS
    #define NOCOMM
    #define NOKANJI
    #define NOHELP
    #define NOPROFILER
    #define NODEFERWINDOWPOS
    #define NOMCX

// Types required before/after windows.h inclusion (normally provided by the
// excluded winuser/wingdi sections but referenced by ole2/mmreg/miniaudio)
typedef struct tagMSG *LPMSG;

#include <windows.h>

typedef struct tagBITMAPINFOHEADER {
    DWORD biSize;
    LONG  biWidth;
    LONG  biHeight;
    WORD  biPlanes;
    WORD  biBitCount;
    DWORD biCompression;
    DWORD biSizeImage;
    LONG  biXPelsPerMeter;
    LONG  biYPelsPerMeter;
    DWORD biClrUsed;
    DWORD biClrImportant;
} BITMAPINFOHEADER, *PBITMAPINFOHEADER;

#include <objbase.h>
#include <mmreg.h>
#include <mmsystem.h>

// COM threading model used by miniaudio: apartment threaded
#define MA_COINIT_VALUE  2
#endif

#define MINIAUDIO_IMPLEMENTATION
#include "external/miniaudio.h"     // Audio device initialization and management

// WARNING: miniaudio pulls in windows.h, which #defines PlaySound to PlaySoundA/W
#if defined(PlaySound)
    #undef PlaySound
#endif

#if defined(SUPPORT_FILEFORMAT_WAV)
    #define DRWAV_MALLOC RL_MALLOC
    #define DRWAV_REALLOC RL_REALLOC
    #define DRWAV_FREE RL_FREE

    #define DR_WAV_IMPLEMENTATION
    #include "external/dr_wav.h"    // WAV loading/saving functions
#endif

#if defined(SUPPORT_FILEFORMAT_OGG)
    // NOTE: stb_vorbis.c is a full implementation file, it must be included exactly once
    #include "external/stb_vorbis.c"    // OGG loading functions
#endif

#if defined(SUPPORT_FILEFORMAT_MP3)
    #define DRMP3_MALLOC RL_MALLOC
    #define DRMP3_REALLOC RL_REALLOC
    #define DRMP3_FREE RL_FREE

    #define DR_MP3_IMPLEMENTATION
    #include "external/dr_mp3.h"    // MP3 loading functions
#endif

#if defined(SUPPORT_FILEFORMAT_FLAC)
    #define DRFLAC_MALLOC RL_MALLOC
    #define DRFLAC_REALLOC RL_REALLOC
    #define DRFLAC_FREE RL_FREE

    #define DR_FLAC_IMPLEMENTATION
    #define DR_FLAC_NO_WIN32_IO
    #include "external/dr_flac.h"   // FLAC loading functions
#endif

#if defined(SUPPORT_FILEFORMAT_QOA)
    #define QOA_MALLOC RL_MALLOC
    #define QOA_FREE RL_FREE

    #define QOA_IMPLEMENTATION
    #include "external/qoa.h"       // QOA loading/saving functions
    #include "external/qoaplay.c"   // QOA stream playing helper functions
#endif

#if defined(SUPPORT_FILEFORMAT_XM)
    #define JARXM_MALLOC RL_MALLOC
    #define JARXM_FREE RL_FREE

    #define JAR_XM_IMPLEMENTATION
    #include "external/jar_xm.h"    // XM loading functions
#endif

#if defined(SUPPORT_FILEFORMAT_MOD)
    #define JARMOD_MALLOC RL_MALLOC
    #define JARMOD_FREE RL_FREE

    #define JAR_MOD_IMPLEMENTATION
    #include "external/jar_mod.h"   // MOD loading functions
#endif

//----------------------------------------------------------------------------------
// Types and Structures Definition
//----------------------------------------------------------------------------------
// Audio buffer usage: how the underlying PCM data is consumed by the mixer
typedef enum {
    AUDIO_BUFFER_USAGE_STATIC = 0,  // Whole sound resident in memory, read once (Sound)
    AUDIO_BUFFER_USAGE_STREAM       // Double-buffered, refilled while playing (Music/AudioStream)
} AudioBufferUsage;

// Music context type: which decoder owns Music.ctxData
typedef enum {
    MUSIC_AUDIO_NONE = 0,   // No audio context loaded
    MUSIC_AUDIO_WAV,        // WAV audio context
    MUSIC_AUDIO_OGG,        // OGG audio context
    MUSIC_AUDIO_FLAC,       // FLAC audio context
    MUSIC_AUDIO_MP3,        // MP3 audio context
    MUSIC_AUDIO_QOA,        // QOA audio context
    MUSIC_MODULE_XM,        // XM module audio context
    MUSIC_MODULE_MOD        // MOD module audio context
} MusicContextType;

// Audio buffer, internal representation of anything playable
struct rAudioBuffer {
    ma_data_converter converter;    // Audio data converter (input format -> mixing format)

    AudioCallback callback;         // Audio buffer callback for buffer filling on audio threads
    rAudioProcessor *processor;     // Audio processors chain attached to this buffer

    float volume;                   // Audio buffer volume
    float pitch;                    // Audio buffer pitch
    float pan;                      // Audio buffer pan (0.0f to 1.0f, 0.5f is centered, 1.0f is full left)

    bool playing;                   // Audio buffer state: AUDIO_PLAYING
    bool paused;                    // Audio buffer state: AUDIO_PAUSED
    bool looping;                   // Audio buffer looping, always true for AudioStreams
    int usage;                      // Audio buffer usage mode: static or stream

    bool isSubBufferProcessed[2];   // SubBuffer processed (virtual double buffer)
    unsigned int sizeInFrames;      // Total buffer size in frames
    unsigned int frameCursorPos;    // Frame cursor position
    unsigned int framesProcessed;   // Total frames processed in this buffer (required for play timing)

    unsigned char *data;            // Data buffer, on music stream keeps filling

    rAudioBuffer *next;             // Next audio buffer on the list
    rAudioBuffer *prev;             // Previous audio buffer on the list
};

// Audio processor, node of a double linked list of user callbacks
struct rAudioProcessor {
    AudioCallback process;          // Processor callback function
    rAudioProcessor *next;          // Next audio processor on the list
    rAudioProcessor *prev;          // Previous audio processor on the list
};

typedef struct rAudioBuffer AudioBuffer;    // Internal short name, matches raylib naming

// Global audio context
typedef struct AudioData {
    struct {
        ma_context context;         // miniaudio context data
        ma_device device;           // miniaudio device
        ma_mutex lock;              // miniaudio mutex lock
        bool isReady;               // Check if audio device is ready
        size_t pcmBufferSize;       // Pre-allocated buffer size
        void *pcmBuffer;            // Pre-allocated buffer to read audio data from file/memory
    } System;
    struct {
        AudioBuffer *first;         // Pointer to first AudioBuffer in the list
        AudioBuffer *last;          // Pointer to last AudioBuffer in the list
        int defaultSize;            // Default audio buffer size for audio streams
    } Buffer;
    rAudioProcessor *mixedProcessor;    // Processors chain applied to the final mix
} AudioData;

//----------------------------------------------------------------------------------
// Global Variables Definition
//----------------------------------------------------------------------------------
static AudioData AUDIO = {          // Global AUDIO context
    .Buffer.defaultSize = 0,
    .mixedProcessor = NULL
};

//----------------------------------------------------------------------------------
// Module specific Functions Declaration
//----------------------------------------------------------------------------------
static void OnLog(void *pUserData, ma_uint32 level, const char *pMessage);
static void OnSendAudioDataToDevice(ma_device *pDevice, void *pFramesOut, const void *pFramesInput, ma_uint32 frameCount);
static void MixAudioFrames(float *framesOut, const float *framesIn, ma_uint32 frameCount, AudioBuffer *buffer);

static AudioBuffer *LoadAudioBuffer(ma_format format, ma_uint32 channels, ma_uint32 sampleRate, ma_uint32 sizeInFrames, int usage);
static void UnloadAudioBuffer(AudioBuffer *buffer);
static bool IsAudioBufferPlaying(AudioBuffer *buffer);
static void PlayAudioBuffer(AudioBuffer *buffer);
static void StopAudioBuffer(AudioBuffer *buffer);
static void PauseAudioBuffer(AudioBuffer *buffer);
static void ResumeAudioBuffer(AudioBuffer *buffer);
static void SetAudioBufferVolume(AudioBuffer *buffer, float volume);
static void SetAudioBufferPitch(AudioBuffer *buffer, float pitch);
static void SetAudioBufferPan(AudioBuffer *buffer, float pan);
static void TrackAudioBuffer(AudioBuffer *buffer);
static void UntrackAudioBuffer(AudioBuffer *buffer);

static ma_uint32 ReadAudioBufferFramesInInternalFormat(AudioBuffer *audioBuffer, void *framesOut, ma_uint32 frameCount);
static ma_uint32 ReadAudioBufferFramesInMixingFormat(AudioBuffer *audioBuffer, float *framesOut, ma_uint32 frameCount);

static void AttachProcessor(rAudioProcessor **list, AudioCallback process);
static void DetachProcessor(rAudioProcessor **list, AudioCallback process);

static ma_format GetMiniaudioFormat(unsigned int sampleSize);
static bool IsSampleSizeValid(unsigned int sampleSize);
static void GetFileExtLower(char *out, size_t outSize, const char *fileType);
static float ClampFloat(float value, float min, float max);
static void AudioLock(void);
static void AudioUnlock(void);
static void UnloadMusicContext(int ctxType, void *ctxData);
static void RewindMusicContext(int ctxType, void *ctxData);

//----------------------------------------------------------------------------------
// Module Functions Definition - Audio Device management
//----------------------------------------------------------------------------------

// Initialize audio device and context
void InitAudioDevice(void)
{
    if (AUDIO.System.isReady)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Device already initialized");
        return;
    }

    // Init audio context
    ma_context_config ctxConfig = ma_context_config_init();
    ma_result result = ma_context_init(NULL, 0, &ctxConfig, &AUDIO.System.context);
    if (result != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to initialize context");
        return;
    }

    ma_log_register_callback(ma_context_get_log(&AUDIO.System.context), ma_log_callback_init(OnLog, NULL));

    // Init audio device
    // NOTE: Using the default device, format is floating point because it simplifies mixing
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.pDeviceID = NULL;   // NULL for the default playback device
    config.playback.format = AUDIO_DEVICE_FORMAT;
    config.playback.channels = AUDIO_DEVICE_CHANNELS;
    config.capture.pDeviceID = NULL;    // NULL for the default capture device
    config.capture.format = ma_format_s16;
    config.capture.channels = 1;
    config.sampleRate = AUDIO_DEVICE_SAMPLE_RATE;
    config.periodSizeInFrames = AUDIO_DEVICE_PERIOD_SIZE_IN_FRAMES;
    config.dataCallback = OnSendAudioDataToDevice;
    config.pUserData = NULL;

    result = ma_device_init(&AUDIO.System.context, &config, &AUDIO.System.device);
    if (result != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to initialize playback device");
        ma_context_uninit(&AUDIO.System.context);
        return;
    }

    // Mixing happens on a separate thread which means we need to synchronize
    // NOTE: A mutex is not real-time safe but it keeps the implementation simple and correct
    if (ma_mutex_init(&AUDIO.System.lock) != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to create mutex for mixing");
        ma_device_uninit(&AUDIO.System.device);
        ma_context_uninit(&AUDIO.System.context);
        return;
    }

    // Keep the device running the whole time
    result = ma_device_start(&AUDIO.System.device);
    if (result != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to start playback device");
        ma_mutex_uninit(&AUDIO.System.lock);
        ma_device_uninit(&AUDIO.System.device);
        ma_context_uninit(&AUDIO.System.context);
        return;
    }

    TRACELOG(LOG_INFO, "AUDIO: Device initialized successfully");
    TRACELOG(LOG_INFO, "    > Backend:       miniaudio / %s", ma_get_backend_name(AUDIO.System.context.backend));
    TRACELOG(LOG_INFO, "    > Format:        %s -> %s", ma_get_format_name(AUDIO.System.device.playback.format), ma_get_format_name(AUDIO.System.device.playback.internalFormat));
    TRACELOG(LOG_INFO, "    > Channels:      %d -> %d", AUDIO.System.device.playback.channels, AUDIO.System.device.playback.internalChannels);
    TRACELOG(LOG_INFO, "    > Sample rate:   %d -> %d", AUDIO.System.device.sampleRate, AUDIO.System.device.playback.internalSampleRate);
    TRACELOG(LOG_INFO, "    > Periods size:  %d", AUDIO.System.device.playback.internalPeriodSizeInFrames*AUDIO.System.device.playback.internalPeriods);

    AUDIO.System.isReady = true;
}

// Close the audio device for all contexts
void CloseAudioDevice(void)
{
    if (!AUDIO.System.isReady)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Device could not be closed, not currently initialized");
        return;
    }

    // NOTE: Device is stopped first so the mixing callback can not run while buffers go away
    ma_device_uninit(&AUDIO.System.device);
    ma_mutex_uninit(&AUDIO.System.lock);
    ma_context_uninit(&AUDIO.System.context);

    AUDIO.System.isReady = false;
    AUDIO.Buffer.first = NULL;
    AUDIO.Buffer.last = NULL;

    RL_FREE(AUDIO.System.pcmBuffer);
    AUDIO.System.pcmBuffer = NULL;
    AUDIO.System.pcmBufferSize = 0;

    TRACELOG(LOG_INFO, "AUDIO: Device closed successfully");
}

// Check if device has been initialized successfully
bool IsAudioDeviceReady(void)
{
    return AUDIO.System.isReady;
}

// Set master volume (listener)
void SetMasterVolume(float volume)
{
    if (!AUDIO.System.isReady) return;

    ma_device_set_master_volume(&AUDIO.System.device, ClampFloat(volume, 0.0f, 1.0f));
}

// Get master volume (listener)
float GetMasterVolume(void)
{
    float volume = 0.0f;
    if (AUDIO.System.isReady) ma_device_get_master_volume(&AUDIO.System.device, &volume);

    return volume;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Audio Buffer management (internal)
//----------------------------------------------------------------------------------

// Initialize a new audio buffer (filled with silence)
// NOTE: sizeInFrames == 0 creates a buffer without data (used for sound aliases)
static AudioBuffer *LoadAudioBuffer(ma_format format, ma_uint32 channels, ma_uint32 sampleRate, ma_uint32 sizeInFrames, int usage)
{
    if (!AUDIO.System.isReady)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Buffer could not be created, device not initialized");
        return NULL;
    }

    if ((channels == 0) || (sampleRate == 0))
    {
        TRACELOG(LOG_WARNING, "AUDIO: Buffer could not be created, invalid format (%i channels, %i Hz)", channels, sampleRate);
        return NULL;
    }

    AudioBuffer *audioBuffer = (AudioBuffer *)RL_CALLOC(1, sizeof(AudioBuffer));
    if (audioBuffer == NULL)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to allocate memory for buffer");
        return NULL;
    }

    if (sizeInFrames > 0)
    {
        audioBuffer->data = (unsigned char *)RL_CALLOC(sizeInFrames*channels*ma_get_bytes_per_sample(format), 1);

        if (audioBuffer->data == NULL)
        {
            TRACELOG(LOG_WARNING, "AUDIO: Failed to allocate memory for buffer data");
            RL_FREE(audioBuffer);
            return NULL;
        }
    }

    // Audio data runs through a format converter to reach the mixing format
    ma_data_converter_config converterConfig = ma_data_converter_config_init(format, AUDIO_DEVICE_FORMAT, channels, AUDIO_DEVICE_CHANNELS, sampleRate, AUDIO.System.device.sampleRate);
    converterConfig.allowDynamicSampleRate = MA_TRUE;   // Required for pitch shifts

    ma_result result = ma_data_converter_init(&converterConfig, NULL, &audioBuffer->converter);
    if (result != MA_SUCCESS)
    {
        TRACELOG(LOG_WARNING, "AUDIO: Failed to create data conversion pipeline");
        RL_FREE(audioBuffer->data);
        RL_FREE(audioBuffer);
        return NULL;
    }

    audioBuffer->volume = 1.0f;
    audioBuffer->pitch = 1.0f;
    audioBuffer->pan = 0.5f;

    audioBuffer->callback = NULL;
    audioBuffer->processor = NULL;

    audioBuffer->playing = false;
    audioBuffer->paused = false;
    audioBuffer->looping = false;

    audioBuffer->usage = usage;
    audioBuffer->frameCursorPos = 0;
    audioBuffer->framesProcessed = 0;
    audioBuffer->sizeInFrames = sizeInFrames;

    // Buffers should be marked as processed by default so that a call to
    // UpdateAudioStream() immediately after initialization works correctly
    audioBuffer->isSubBufferProcessed[0] = true;
    audioBuffer->isSubBufferProcessed[1] = true;

    TrackAudioBuffer(audioBuffer);

    return audioBuffer;
}

// Delete an audio buffer
static void UnloadAudioBuffer(AudioBuffer *buffer)
{
    if (buffer == NULL) return;

    ma_data_converter_uninit(&buffer->converter, NULL);
    UntrackAudioBuffer(buffer);
    RL_FREE(buffer->data);
    RL_FREE(buffer);
}

// Check if an audio buffer is playing
static bool IsAudioBufferPlaying(AudioBuffer *buffer)
{
    if (buffer == NULL) return false;

    return (buffer->playing && !buffer->paused);
}

// Play an audio buffer
// NOTE: Buffer is restarted to the start of the data
static void PlayAudioBuffer(AudioBuffer *buffer)
{
    if (buffer == NULL) return;

    buffer->playing = true;
    buffer->paused = false;
    buffer->frameCursorPos = 0;
}

// Stop an audio buffer
static void StopAudioBuffer(AudioBuffer *buffer)
{
    if (buffer == NULL) return;

    // NOTE: Unlike raylib, a paused buffer is also reset, stopping must always be effective
    buffer->playing = false;
    buffer->paused = false;
    buffer->frameCursorPos = 0;
    buffer->framesProcessed = 0;
    buffer->isSubBufferProcessed[0] = true;
    buffer->isSubBufferProcessed[1] = true;
}

// Pause an audio buffer
static void PauseAudioBuffer(AudioBuffer *buffer)
{
    if (buffer != NULL) buffer->paused = true;
}

// Resume an audio buffer
static void ResumeAudioBuffer(AudioBuffer *buffer)
{
    if (buffer != NULL) buffer->paused = false;
}

// Set volume for an audio buffer
static void SetAudioBufferVolume(AudioBuffer *buffer, float volume)
{
    if (buffer != NULL) buffer->volume = ClampFloat(volume, 0.0f, 1.0f);
}

// Set pitch for an audio buffer
// NOTE: Pitch is implemented by changing the resampling ratio of the data converter
static void SetAudioBufferPitch(AudioBuffer *buffer, float pitch)
{
    if ((buffer == NULL) || (pitch <= 0.0f)) return;

    // Pitching is just an adjustment of the sample rate
    // Note that this changes the duration of the sound:
    //  - higher pitches will make the sound faster
    //  - lower pitches make it slower
    ma_uint32 outputSampleRate = (ma_uint32)((float)buffer->converter.sampleRateOut/pitch);
    if (outputSampleRate == 0) outputSampleRate = 1;

    ma_data_converter_set_rate(&buffer->converter, buffer->converter.sampleRateIn, outputSampleRate);

    buffer->pitch = pitch;
}

// Set pan for an audio buffer (internal convention: 0.0f full right, 0.5f center, 1.0f full left)
static void SetAudioBufferPan(AudioBuffer *buffer, float pan)
{
    if (buffer != NULL) buffer->pan = ClampFloat(pan, 0.0f, 1.0f);
}

// Track audio buffer to linked list next position
static void TrackAudioBuffer(AudioBuffer *buffer)
{
    AudioLock();
    {
        if (AUDIO.Buffer.first == NULL) AUDIO.Buffer.first = buffer;
        else
        {
            AUDIO.Buffer.last->next = buffer;
            buffer->prev = AUDIO.Buffer.last;
        }

        AUDIO.Buffer.last = buffer;
    }
    AudioUnlock();
}

// Untrack audio buffer from linked list
static void UntrackAudioBuffer(AudioBuffer *buffer)
{
    AudioLock();
    {
        if (buffer->prev == NULL) AUDIO.Buffer.first = buffer->next;
        else buffer->prev->next = buffer->next;

        if (buffer->next == NULL) AUDIO.Buffer.last = buffer->prev;
        else buffer->next->prev = buffer->prev;

        buffer->prev = NULL;
        buffer->next = NULL;
    }
    AudioUnlock();
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Wave/Sound loading/unloading
//----------------------------------------------------------------------------------

// Load wave data from file
Wave LoadWave(const char *fileName)
{
    Wave wave = { 0 };

    // Loading file to memory
    int dataSize = 0;
    unsigned char *fileData = LoadFileData(fileName, &dataSize);

    // Loading wave from memory data
    if (fileData != NULL) wave = LoadWaveFromMemory(GetFileExtension(fileName), fileData, dataSize);

    UnloadFileData(fileData);

    return wave;
}

// Load wave from memory buffer, fileType refers to extension: i.e. ".wav"
// WARNING: File extension must be provided in lower-case
Wave LoadWaveFromMemory(const char *fileType, const unsigned char *fileData, int dataSize)
{
    Wave wave = { 0 };

    if ((fileType == NULL) || (fileData == NULL) || (dataSize <= 0))
    {
        TRACELOG(LOG_WARNING, "WAVE: Invalid file data provided");
        return wave;
    }

    char fileExtLower[16] = { 0 };
    GetFileExtLower(fileExtLower, sizeof(fileExtLower), fileType);

    if (false) { }
#if defined(SUPPORT_FILEFORMAT_WAV)
    else if (strcmp(fileExtLower, ".wav") == 0)
    {
        drwav wav = { 0 };
        bool success = drwav_init_memory(&wav, fileData, (size_t)dataSize, NULL);

        if (success)
        {
            wave.frameCount = (unsigned int)wav.totalPCMFrameCount;
            wave.sampleRate = wav.sampleRate;
            wave.sampleSize = 16;
            wave.channels = wav.channels;
            wave.data = RL_MALLOC(wave.frameCount*wave.channels*sizeof(short));

            // NOTE: We are forcing conversion to 16bit sample size on reading
            if (wave.data != NULL) drwav_read_pcm_frames_s16(&wav, wave.frameCount, (short *)wave.data);

            drwav_uninit(&wav);
        }
        else TRACELOG(LOG_WARNING, "WAVE: Failed to load WAV data");
    }
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
    else if (strcmp(fileExtLower, ".ogg") == 0)
    {
        stb_vorbis *oggData = stb_vorbis_open_memory((const unsigned char *)fileData, dataSize, NULL, NULL);

        if (oggData != NULL)
        {
            stb_vorbis_info info = stb_vorbis_get_info(oggData);

            wave.sampleRate = info.sample_rate;
            wave.sampleSize = 16;
            wave.channels = info.channels;
            wave.frameCount = (unsigned int)stb_vorbis_stream_length_in_samples(oggData);
            wave.data = RL_MALLOC(wave.frameCount*wave.channels*sizeof(short));

            // NOTE: Get the number of samples to process (be careful! we ask for number of shorts, not bytes!)
            if (wave.data != NULL) stb_vorbis_get_samples_short_interleaved(oggData, info.channels, (short *)wave.data, (int)(wave.frameCount*wave.channels));

            stb_vorbis_close(oggData);
        }
        else TRACELOG(LOG_WARNING, "WAVE: Failed to load OGG data");
    }
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
    else if (strcmp(fileExtLower, ".mp3") == 0)
    {
        drmp3_config config = { 0 };
        unsigned long long int totalFrameCount = 0;

        // NOTE: We are forcing conversion to 32bit float sample size on reading
        wave.data = drmp3_open_memory_and_read_pcm_frames_f32(fileData, (size_t)dataSize, &config, &totalFrameCount, NULL);

        if (wave.data != NULL)
        {
            wave.channels = config.channels;
            wave.sampleRate = config.sampleRate;
            wave.frameCount = (unsigned int)totalFrameCount;
            wave.sampleSize = 32;
        }
        else TRACELOG(LOG_WARNING, "WAVE: Failed to load MP3 data");
    }
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
    else if (strcmp(fileExtLower, ".flac") == 0)
    {
        unsigned long long int totalFrameCount = 0;

        // NOTE: We are forcing conversion to 16bit sample size on reading
        wave.data = drflac_open_memory_and_read_pcm_frames_s16(fileData, (size_t)dataSize, &wave.channels, &wave.sampleRate, &totalFrameCount, NULL);

        if (wave.data != NULL)
        {
            wave.frameCount = (unsigned int)totalFrameCount;
            wave.sampleSize = 16;
        }
        else TRACELOG(LOG_WARNING, "WAVE: Failed to load FLAC data");
    }
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
    else if (strcmp(fileExtLower, ".qoa") == 0)
    {
        qoa_desc qoa = { 0 };

        wave.data = qoa_decode(fileData, dataSize, &qoa);

        if (wave.data != NULL)
        {
            wave.channels = qoa.channels;
            wave.sampleRate = qoa.samplerate;
            wave.frameCount = qoa.samples;
            wave.sampleSize = 16;
        }
        else TRACELOG(LOG_WARNING, "WAVE: Failed to load QOA data");
    }
#endif
    else TRACELOG(LOG_WARNING, "WAVE: Data format not supported");

    if (wave.data == NULL)
    {
        // Make sure a partially filled descriptor is never returned
        Wave empty = { 0 };
        return empty;
    }

    TRACELOG(LOG_INFO, "WAVE: Data loaded successfully (%i Hz, %i bit, %s)", wave.sampleRate, wave.sampleSize, (wave.channels == 1)? "Mono" : "Stereo");

    return wave;
}

// Checks if wave data is valid (data loaded and parameters)
bool IsWaveValid(Wave wave)
{
    return ((wave.data != NULL) &&      // Validate wave data available
            (wave.frameCount > 0) &&    // Validate frame count
            (wave.sampleRate > 0) &&    // Validate sample rate
            (wave.sampleSize > 0) &&    // Validate sample size
            (wave.channels > 0));       // Validate number of channels
}

// Load sound from file
// NOTE: The entire file is loaded to memory to be played (no-streaming)
Sound LoadSound(const char *fileName)
{
    Wave wave = LoadWave(fileName);

    Sound sound = LoadSoundFromWave(wave);

    UnloadWave(wave);   // Sound is loaded, we can unload wave

    return sound;
}

// Load sound from wave data
// NOTE: Wave data must be unallocated manually
Sound LoadSoundFromWave(Wave wave)
{
    Sound sound = { 0 };

    if (!IsWaveValid(wave))
    {
        TRACELOG(LOG_WARNING, "SOUND: Provided wave data is not valid");
        return sound;
    }

    if (!IsSampleSizeValid(wave.sampleSize))
    {
        TRACELOG(LOG_WARNING, "SOUND: Sample size not supported: %i bit", wave.sampleSize);
        return sound;
    }

    // When using miniaudio we need to do our own mixing
    // To simplify this we need convert the format of each sound to be consistent with
    // the format used to open the playback device, conversion is done on the loading stage
    ma_format formatIn = GetMiniaudioFormat(wave.sampleSize);
    ma_uint32 frameCountIn = wave.frameCount;

    ma_uint32 frameCount = (ma_uint32)ma_convert_frames(NULL, 0, AUDIO_DEVICE_FORMAT, AUDIO_DEVICE_CHANNELS, AUDIO.System.device.sampleRate, NULL, frameCountIn, formatIn, wave.channels, wave.sampleRate);
    if (frameCount == 0)
    {
        TRACELOG(LOG_WARNING, "SOUND: Failed to get frame count for format conversion");
        return sound;
    }

    AudioBuffer *audioBuffer = LoadAudioBuffer(AUDIO_DEVICE_FORMAT, AUDIO_DEVICE_CHANNELS, AUDIO.System.device.sampleRate, frameCount, AUDIO_BUFFER_USAGE_STATIC);
    if (audioBuffer == NULL)
    {
        TRACELOG(LOG_WARNING, "SOUND: Failed to create buffer");
        return sound;
    }

    frameCount = (ma_uint32)ma_convert_frames(audioBuffer->data, audioBuffer->sizeInFrames, AUDIO_DEVICE_FORMAT, AUDIO_DEVICE_CHANNELS, AUDIO.System.device.sampleRate, wave.data, frameCountIn, formatIn, wave.channels, wave.sampleRate);
    if (frameCount == 0)
    {
        TRACELOG(LOG_WARNING, "SOUND: Failed format conversion");
        UnloadAudioBuffer(audioBuffer);
        return sound;
    }

    sound.frameCount = frameCount;
    sound.stream.sampleRate = AUDIO.System.device.sampleRate;
    sound.stream.sampleSize = 32;
    sound.stream.channels = AUDIO_DEVICE_CHANNELS;
    sound.stream.buffer = audioBuffer;

    return sound;
}

// Create a new sound that shares the same sample data as the source sound, does not own the sound data
Sound LoadSoundAlias(Sound source)
{
    Sound sound = { 0 };

    if ((source.stream.buffer == NULL) || (source.stream.buffer->data == NULL))
    {
        TRACELOG(LOG_WARNING, "SOUND: Provided source sound is not valid");
        return sound;
    }

    AudioBuffer *audioBuffer = LoadAudioBuffer(AUDIO_DEVICE_FORMAT, AUDIO_DEVICE_CHANNELS, AUDIO.System.device.sampleRate, 0, AUDIO_BUFFER_USAGE_STATIC);
    if (audioBuffer == NULL)
    {
        TRACELOG(LOG_WARNING, "SOUND: Failed to create buffer for alias");
        return sound;
    }

    audioBuffer->sizeInFrames = source.stream.buffer->sizeInFrames;
    audioBuffer->volume = source.stream.buffer->volume;
    audioBuffer->data = source.stream.buffer->data;     // NOTE: Data is shared, not owned

    sound.frameCount = source.frameCount;
    sound.stream.sampleRate = source.stream.sampleRate;
    sound.stream.sampleSize = source.stream.sampleSize;
    sound.stream.channels = source.stream.channels;
    sound.stream.buffer = audioBuffer;

    return sound;
}

// Checks if a sound is valid (data loaded and buffers initialized)
bool IsSoundValid(Sound sound)
{
    return ((sound.frameCount > 0) &&           // Validate frame count
            (sound.stream.buffer != NULL) &&    // Validate stream buffer
            (sound.stream.sampleRate > 0) &&    // Validate sample rate
            (sound.stream.sampleSize > 0) &&    // Validate sample size
            (sound.stream.channels > 0));       // Validate number of channels
}

// Unload wave data
void UnloadWave(Wave wave)
{
    RL_FREE(wave.data);
}

// Unload sound
void UnloadSound(Sound sound)
{
    UnloadAudioBuffer(sound.stream.buffer);
}

// Unload a sound alias (does not deallocate sample data)
void UnloadSoundAlias(Sound alias)
{
    if (alias.stream.buffer == NULL) return;

    // Untrack and unload just the sound buffer, but not the shared data
    ma_data_converter_uninit(&alias.stream.buffer->converter, NULL);
    UntrackAudioBuffer(alias.stream.buffer);
    RL_FREE(alias.stream.buffer);
}

// Update sound buffer with new data
// NOTE: data format must match the sound stream format (default: 32bit float, stereo)
void UpdateSound(Sound sound, const void *data, int sampleCount)
{
    if ((sound.stream.buffer == NULL) || (data == NULL) || (sampleCount <= 0)) return;

    StopAudioBuffer(sound.stream.buffer);

    ma_uint32 frameCount = (ma_uint32)sampleCount;
    if (frameCount > sound.stream.buffer->sizeInFrames)
    {
        TRACELOG(LOG_WARNING, "SOUND: Attempting to write too many frames to buffer, clamping");
        frameCount = sound.stream.buffer->sizeInFrames;
    }

    AudioLock();
    {
        memcpy(sound.stream.buffer->data, data, frameCount*ma_get_bytes_per_frame(sound.stream.buffer->converter.formatIn, sound.stream.buffer->converter.channelsIn));
    }
    AudioUnlock();
}

// Export wave data to file, returns true on success
bool ExportWave(Wave wave, const char *fileName)
{
    bool success = false;

    if (!IsWaveValid(wave) || (fileName == NULL)) return false;

    if (false) { }
#if defined(SUPPORT_FILEFORMAT_WAV)
    else if (IsFileExtension(fileName, ".wav"))
    {
        drwav wav = { 0 };
        drwav_data_format format = { 0 };
        format.container = drwav_container_riff;
        format.format = (wave.sampleSize == 32)? DR_WAVE_FORMAT_IEEE_FLOAT : DR_WAVE_FORMAT_PCM;
        format.channels = wave.channels;
        format.sampleRate = wave.sampleRate;
        format.bitsPerSample = wave.sampleSize;

        void *fileData = NULL;
        size_t fileDataSize = 0;

        if (drwav_init_memory_write(&wav, &fileData, &fileDataSize, &format, NULL))
        {
            drwav_write_pcm_frames(&wav, wave.frameCount, wave.data);
            drwav_result result = drwav_uninit(&wav);

            if (result == DRWAV_SUCCESS) success = SaveFileData(fileName, (unsigned char *)fileData, (int)fileDataSize);
        }

        drwav_free(fileData, NULL);
    }
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
    else if (IsFileExtension(fileName, ".qoa"))
    {
        if (wave.sampleSize == 16)
        {
            qoa_desc qoa = {
                .channels = wave.channels,
                .samplerate = wave.sampleRate,
                .samples = wave.frameCount
            };

            int bytesWritten = qoa_write(fileName, (const short *)wave.data, &qoa);
            if (bytesWritten > 0) success = true;
        }
        else TRACELOG(LOG_WARNING, "AUDIO: QOA export expects 16bit sample size");
    }
#endif
    else if (IsFileExtension(fileName, ".raw"))
    {
        // Export raw sample data (without header)
        // NOTE: It's up to the user to track wave parameters
        success = SaveFileData(fileName, wave.data, (int)(wave.frameCount*wave.channels*wave.sampleSize/8));
    }

    if (success) TRACELOG(LOG_INFO, "FILEIO: [%s] Wave data exported successfully", fileName);
    else TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to export wave data", fileName);

    return success;
}

// Export wave sample data to code (.h), returns true on success
bool ExportWaveAsCode(Wave wave, const char *fileName)
{
#ifndef TEXT_BYTES_PER_LINE
    #define TEXT_BYTES_PER_LINE     20
#endif

    if (!IsWaveValid(wave) || (fileName == NULL)) return false;

    const int waveDataSize = (int)(wave.frameCount*wave.channels*wave.sampleSize/8);

    // NOTE: Text data buffer size is estimated considering wave data size in bytes
    // and requiring 6 char bytes for every byte: "0x00, "
    char *txtData = (char *)RL_CALLOC((size_t)waveDataSize*6 + 2000, sizeof(char));
    if (txtData == NULL) return false;

    int byteCount = 0;
    byteCount += sprintf(txtData + byteCount, "\n//////////////////////////////////////////////////////////////////////////////////\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "// WaveAsCode exporter v1.1 - Wave data exported as an array of bytes           //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "// more info and bugs-report:  github.com/raysan5/raylib                        //\n");
    byteCount += sprintf(txtData + byteCount, "// feedback and support:       ray[at]raylib.com                                //\n");
    byteCount += sprintf(txtData + byteCount, "//                                                                              //\n");
    byteCount += sprintf(txtData + byteCount, "//////////////////////////////////////////////////////////////////////////////////\n\n");

    // Get file name from path and convert variable name to uppercase
    char varFileName[256] = { 0 };
    strncpy(varFileName, GetFileNameWithoutExt(fileName), sizeof(varFileName) - 1);
    for (int i = 0; varFileName[i] != '\0'; i++)
    {
        if ((varFileName[i] >= 'a') && (varFileName[i] <= 'z')) varFileName[i] = (char)(varFileName[i] - 32);
    }

    byteCount += sprintf(txtData + byteCount, "// Wave data information\n");
    byteCount += sprintf(txtData + byteCount, "#define %s_FRAME_COUNT      %u\n", varFileName, wave.frameCount);
    byteCount += sprintf(txtData + byteCount, "#define %s_SAMPLE_RATE      %u\n", varFileName, wave.sampleRate);
    byteCount += sprintf(txtData + byteCount, "#define %s_SAMPLE_SIZE      %u\n", varFileName, wave.sampleSize);
    byteCount += sprintf(txtData + byteCount, "#define %s_CHANNELS         %u\n\n", varFileName, wave.channels);

    // Write wave data as an array of bytes
    byteCount += sprintf(txtData + byteCount, "static unsigned char %s_DATA[%i] = { ", varFileName, waveDataSize);
    for (int i = 0; i < (waveDataSize - 1); i++)
    {
        byteCount += sprintf(txtData + byteCount, ((i%TEXT_BYTES_PER_LINE == 0)? "0x%x,\n" : "0x%x, "), ((unsigned char *)wave.data)[i]);
    }
    byteCount += sprintf(txtData + byteCount, "0x%x };\n", ((unsigned char *)wave.data)[waveDataSize - 1]);

    // NOTE: Text data length exported is determined by '\0' (NULL) character
    bool success = SaveFileText(fileName, txtData);

    RL_FREE(txtData);

    if (success) TRACELOG(LOG_INFO, "FILEIO: [%s] Wave as code exported successfully", fileName);
    else TRACELOG(LOG_WARNING, "FILEIO: [%s] Failed to export wave as code", fileName);

    return success;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Wave/Sound management
//----------------------------------------------------------------------------------

// Play a sound
void PlaySound(Sound sound)
{
    PlayAudioBuffer(sound.stream.buffer);
}

// Pause a sound
void PauseSound(Sound sound)
{
    PauseAudioBuffer(sound.stream.buffer);
}

// Resume a paused sound
void ResumeSound(Sound sound)
{
    ResumeAudioBuffer(sound.stream.buffer);
}

// Stop reproducing a sound
void StopSound(Sound sound)
{
    StopAudioBuffer(sound.stream.buffer);
}

// Check if a sound is playing
bool IsSoundPlaying(Sound sound)
{
    return IsAudioBufferPlaying(sound.stream.buffer);
}

// Set volume for a sound (1.0 is max level)
void SetSoundVolume(Sound sound, float volume)
{
    SetAudioBufferVolume(sound.stream.buffer, volume);
}

// Set pitch for a sound (1.0 is base level)
void SetSoundPitch(Sound sound, float pitch)
{
    SetAudioBufferPitch(sound.stream.buffer, pitch);
}

// Set pan for a sound (-1.0 left, 0.0 center, 1.0 right)
void SetSoundPan(Sound sound, float pan)
{
    SetAudioBufferPan(sound.stream.buffer, (1.0f - ClampFloat(pan, -1.0f, 1.0f))*0.5f);
}

// Copy a wave to a new wave
Wave WaveCopy(Wave wave)
{
    Wave newWave = { 0 };

    if (!IsWaveValid(wave)) return newWave;

    const unsigned int dataSize = wave.frameCount*wave.channels*wave.sampleSize/8;

    newWave.data = RL_MALLOC(dataSize);
    if (newWave.data == NULL)
    {
        TRACELOG(LOG_WARNING, "WAVE: Failed to allocate memory for wave copy");
        return newWave;
    }

    memcpy(newWave.data, wave.data, dataSize);

    newWave.frameCount = wave.frameCount;
    newWave.sampleRate = wave.sampleRate;
    newWave.sampleSize = wave.sampleSize;
    newWave.channels = wave.channels;

    return newWave;
}

// Crop a wave to defined frames range
// NOTE: Security check in case of out-of-range
void WaveCrop(Wave *wave, int initFrame, int finalFrame)
{
    if ((wave == NULL) || !IsWaveValid(*wave)) return;

    if ((initFrame < 0) || (initFrame >= finalFrame) || (finalFrame > (int)wave->frameCount))
    {
        TRACELOG(LOG_WARNING, "WAVE: Crop range out of bounds");
        return;
    }

    const unsigned int frameSize = wave->channels*wave->sampleSize/8;
    const unsigned int frameCount = (unsigned int)(finalFrame - initFrame);

    void *data = RL_CALLOC(frameCount*frameSize, 1);
    if (data == NULL)
    {
        TRACELOG(LOG_WARNING, "WAVE: Failed to allocate memory for cropped wave");
        return;
    }

    memcpy(data, (unsigned char *)wave->data + ((unsigned int)initFrame*frameSize), frameCount*frameSize);

    RL_FREE(wave->data);
    wave->data = data;
    wave->frameCount = frameCount;
}

// Convert wave data to desired format
void WaveFormat(Wave *wave, int sampleRate, int sampleSize, int channels)
{
    if ((wave == NULL) || !IsWaveValid(*wave)) return;

    if ((sampleRate <= 0) || (channels <= 0) || !IsSampleSizeValid((unsigned int)sampleSize))
    {
        TRACELOG(LOG_WARNING, "WAVE: Requested format not supported (%i Hz, %i bit, %i channels)", sampleRate, sampleSize, channels);
        return;
    }

    const ma_format formatIn = GetMiniaudioFormat(wave->sampleSize);
    const ma_format formatOut = GetMiniaudioFormat((unsigned int)sampleSize);

    const ma_uint32 frameCountIn = wave->frameCount;

    ma_uint32 frameCount = (ma_uint32)ma_convert_frames(NULL, 0, formatOut, (ma_uint32)channels, (ma_uint32)sampleRate, NULL, frameCountIn, formatIn, wave->channels, wave->sampleRate);
    if (frameCount == 0)
    {
        TRACELOG(LOG_WARNING, "WAVE: Failed to get frame count for format conversion");
        return;
    }

    void *data = RL_MALLOC(frameCount*(ma_uint32)channels*(ma_uint32)(sampleSize/8));
    if (data == NULL)
    {
        TRACELOG(LOG_WARNING, "WAVE: Failed to allocate memory for format conversion");
        return;
    }

    frameCount = (ma_uint32)ma_convert_frames(data, frameCount, formatOut, (ma_uint32)channels, (ma_uint32)sampleRate, wave->data, frameCountIn, formatIn, wave->channels, wave->sampleRate);
    if (frameCount == 0)
    {
        TRACELOG(LOG_WARNING, "WAVE: Failed format conversion");
        RL_FREE(data);
        return;
    }

    RL_FREE(wave->data);
    wave->data = data;
    wave->frameCount = frameCount;
    wave->sampleRate = (unsigned int)sampleRate;
    wave->sampleSize = (unsigned int)sampleSize;
    wave->channels = (unsigned int)channels;
}

// Load samples data from wave as a 32bit float data array
// NOTE 1: Returned sample values are normalized to range [-1..1]
// NOTE 2: Sample data allocated should be freed with UnloadWaveSamples()
float *LoadWaveSamples(Wave wave)
{
    if (!IsWaveValid(wave)) return NULL;

    const unsigned int sampleCount = wave.frameCount*wave.channels;

    float *samples = (float *)RL_MALLOC(sampleCount*sizeof(float));
    if (samples == NULL) return NULL;

    if (wave.sampleSize == 8)
    {
        for (unsigned int i = 0; i < sampleCount; i++) samples[i] = (float)(((unsigned char *)wave.data)[i] - 127)/256.0f;
    }
    else if (wave.sampleSize == 16)
    {
        for (unsigned int i = 0; i < sampleCount; i++) samples[i] = (float)(((short *)wave.data)[i])/32767.0f;
    }
    else if (wave.sampleSize == 32) memcpy(samples, wave.data, sampleCount*sizeof(float));
    else
    {
        TRACELOG(LOG_WARNING, "WAVE: Sample size not supported: %i bit", wave.sampleSize);
        RL_FREE(samples);
        return NULL;
    }

    return samples;
}

// Unload samples data loaded with LoadWaveSamples()
void UnloadWaveSamples(float *samples)
{
    RL_FREE(samples);
}

//----------------------------------------------------------------------------------
// Module Functions Definition - Music loading and stream playing
//----------------------------------------------------------------------------------

// Load music stream from file
Music LoadMusicStream(const char *fileName)
{
    Music music = { 0 };
    bool musicLoaded = false;

    if (fileName == NULL) return music;

    char fileExtLower[16] = { 0 };
    GetFileExtLower(fileExtLower, sizeof(fileExtLower), GetFileExtension(fileName));

    if (false) { }
#if defined(SUPPORT_FILEFORMAT_WAV)
    else if (strcmp(fileExtLower, ".wav") == 0)
    {
        drwav *ctxWav = (drwav *)RL_CALLOC(1, sizeof(drwav));

        if (ctxWav != NULL)
        {
            music.ctxType = MUSIC_AUDIO_WAV;
            music.ctxData = ctxWav;

            if (drwav_init_file(ctxWav, fileName, NULL))
            {
                int sampleSize = ctxWav->bitsPerSample;
                if (ctxWav->bitsPerSample == 24) sampleSize = 16;   // Forcing conversion to s16 on UpdateMusicStream()

                music.stream = LoadAudioStream(ctxWav->sampleRate, (unsigned int)sampleSize, ctxWav->channels);
                music.frameCount = (unsigned int)ctxWav->totalPCMFrameCount;
                music.looping = true;
                musicLoaded = true;
            }
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
    else if (strcmp(fileExtLower, ".ogg") == 0)
    {
        stb_vorbis *ctxOgg = stb_vorbis_open_filename(fileName, NULL, NULL);

        if (ctxOgg != NULL)
        {
            music.ctxType = MUSIC_AUDIO_OGG;
            music.ctxData = ctxOgg;

            stb_vorbis_info info = stb_vorbis_get_info(ctxOgg);

            // OGG bit rate defaults to 16 bit, it's enough for compressed format
            music.stream = LoadAudioStream(info.sample_rate, 16, info.channels);

            // WARNING: It seems this function returns length in frames, not samples, so we multiply by channels
            music.frameCount = (unsigned int)stb_vorbis_stream_length_in_samples(ctxOgg);
            music.looping = true;
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
    else if (strcmp(fileExtLower, ".mp3") == 0)
    {
        drmp3 *ctxMp3 = (drmp3 *)RL_CALLOC(1, sizeof(drmp3));

        if (ctxMp3 != NULL)
        {
            music.ctxType = MUSIC_AUDIO_MP3;
            music.ctxData = ctxMp3;

            if (drmp3_init_file(ctxMp3, fileName, NULL))
            {
                music.stream = LoadAudioStream(ctxMp3->sampleRate, 32, ctxMp3->channels);
                music.frameCount = (unsigned int)drmp3_get_pcm_frame_count(ctxMp3);
                music.looping = true;
                musicLoaded = true;
            }
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
    else if (strcmp(fileExtLower, ".flac") == 0)
    {
        drflac *ctxFlac = drflac_open_file(fileName, NULL);

        if (ctxFlac != NULL)
        {
            music.ctxType = MUSIC_AUDIO_FLAC;
            music.ctxData = ctxFlac;

            music.stream = LoadAudioStream(ctxFlac->sampleRate, 16, ctxFlac->channels);
            music.frameCount = (unsigned int)ctxFlac->totalPCMFrameCount;
            music.looping = true;
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
    else if (strcmp(fileExtLower, ".qoa") == 0)
    {
        qoaplay_desc *ctxQoa = qoaplay_open(fileName);

        if (ctxQoa != NULL)
        {
            music.ctxType = MUSIC_AUDIO_QOA;
            music.ctxData = ctxQoa;

            // NOTE: qoaplay_decode() provides 32bit float normalized data
            music.stream = LoadAudioStream(ctxQoa->info.samplerate, 32, ctxQoa->info.channels);
            music.frameCount = ctxQoa->info.samples;
            music.looping = true;
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_XM)
    else if (strcmp(fileExtLower, ".xm") == 0)
    {
        jar_xm_context_t *ctxXm = NULL;
        int result = jar_xm_create_context_from_file(&ctxXm, AUDIO.System.device.sampleRate, fileName);

        if ((result == 0) && (ctxXm != NULL))
        {
            music.ctxType = MUSIC_MODULE_XM;
            music.ctxData = ctxXm;

            jar_xm_set_max_loop_count(ctxXm, 0);    // Set infinite number of loops

            // NOTE: Only stereo is supported for XM
            music.stream = LoadAudioStream(AUDIO.System.device.sampleRate, 32, 2);
            music.frameCount = (unsigned int)jar_xm_get_remaining_samples(ctxXm);
            music.looping = true;
            jar_xm_reset(ctxXm);    // Make sure we start at the beginning of the song
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_MOD)
    else if (strcmp(fileExtLower, ".mod") == 0)
    {
        jar_mod_context_t *ctxMod = (jar_mod_context_t *)RL_CALLOC(1, sizeof(jar_mod_context_t));

        if (ctxMod != NULL)
        {
            music.ctxType = MUSIC_MODULE_MOD;
            music.ctxData = ctxMod;

            jar_mod_init(ctxMod);

            if (jar_mod_load_file(ctxMod, fileName) > 0)
            {
                // NOTE: Only stereo is supported for MOD
                music.stream = LoadAudioStream(AUDIO.System.device.sampleRate, 16, 2);
                music.frameCount = (unsigned int)jar_mod_max_samples(ctxMod);
                music.looping = true;
                musicLoaded = true;
            }
        }
    }
#endif
    else TRACELOG(LOG_WARNING, "STREAM: [%s] File format not supported", fileName);

    if (!musicLoaded || (music.stream.buffer == NULL) || (music.frameCount == 0))
    {
        UnloadMusicContext(music.ctxType, music.ctxData);
        UnloadAudioStream(music.stream);

        Music empty = { 0 };
        TRACELOG(LOG_WARNING, "FILEIO: [%s] Music file could not be opened", fileName);
        return empty;
    }

    TRACELOG(LOG_INFO, "FILEIO: [%s] Music file loaded successfully", fileName);
    TRACELOG(LOG_INFO, "    > Sample rate:   %i Hz", music.stream.sampleRate);
    TRACELOG(LOG_INFO, "    > Sample size:   %i bits", music.stream.sampleSize);
    TRACELOG(LOG_INFO, "    > Channels:      %i (%s)", music.stream.channels, (music.stream.channels == 1)? "Mono" : "Stereo");
    TRACELOG(LOG_INFO, "    > Total frames:  %i", music.frameCount);

    return music;
}

// Load music stream from data
// WARNING: Except for QOA/XM/MOD, provided data is NOT copied, it must remain valid
// (and unmodified) for the whole music lifetime, same behaviour as raylib
Music LoadMusicStreamFromMemory(const char *fileType, const unsigned char *data, int dataSize)
{
    Music music = { 0 };
    bool musicLoaded = false;

    if ((fileType == NULL) || (data == NULL) || (dataSize <= 0)) return music;

    char fileExtLower[16] = { 0 };
    GetFileExtLower(fileExtLower, sizeof(fileExtLower), fileType);

    if (false) { }
#if defined(SUPPORT_FILEFORMAT_WAV)
    else if (strcmp(fileExtLower, ".wav") == 0)
    {
        drwav *ctxWav = (drwav *)RL_CALLOC(1, sizeof(drwav));

        if (ctxWav != NULL)
        {
            music.ctxType = MUSIC_AUDIO_WAV;
            music.ctxData = ctxWav;

            if (drwav_init_memory(ctxWav, (const void *)data, (size_t)dataSize, NULL))
            {
                int sampleSize = ctxWav->bitsPerSample;
                if (ctxWav->bitsPerSample == 24) sampleSize = 16;

                music.stream = LoadAudioStream(ctxWav->sampleRate, (unsigned int)sampleSize, ctxWav->channels);
                music.frameCount = (unsigned int)ctxWav->totalPCMFrameCount;
                music.looping = true;
                musicLoaded = true;
            }
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
    else if (strcmp(fileExtLower, ".ogg") == 0)
    {
        stb_vorbis *ctxOgg = stb_vorbis_open_memory((const unsigned char *)data, dataSize, NULL, NULL);

        if (ctxOgg != NULL)
        {
            music.ctxType = MUSIC_AUDIO_OGG;
            music.ctxData = ctxOgg;

            stb_vorbis_info info = stb_vorbis_get_info(ctxOgg);

            music.stream = LoadAudioStream(info.sample_rate, 16, info.channels);
            music.frameCount = (unsigned int)stb_vorbis_stream_length_in_samples(ctxOgg);
            music.looping = true;
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
    else if (strcmp(fileExtLower, ".mp3") == 0)
    {
        drmp3 *ctxMp3 = (drmp3 *)RL_CALLOC(1, sizeof(drmp3));

        if (ctxMp3 != NULL)
        {
            music.ctxType = MUSIC_AUDIO_MP3;
            music.ctxData = ctxMp3;

            if (drmp3_init_memory(ctxMp3, (const void *)data, (size_t)dataSize, NULL))
            {
                music.stream = LoadAudioStream(ctxMp3->sampleRate, 32, ctxMp3->channels);
                music.frameCount = (unsigned int)drmp3_get_pcm_frame_count(ctxMp3);
                music.looping = true;
                musicLoaded = true;
            }
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
    else if (strcmp(fileExtLower, ".flac") == 0)
    {
        drflac *ctxFlac = drflac_open_memory((const void *)data, (size_t)dataSize, NULL);

        if (ctxFlac != NULL)
        {
            music.ctxType = MUSIC_AUDIO_FLAC;
            music.ctxData = ctxFlac;

            music.stream = LoadAudioStream(ctxFlac->sampleRate, 16, ctxFlac->channels);
            music.frameCount = (unsigned int)ctxFlac->totalPCMFrameCount;
            music.looping = true;
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
    else if (strcmp(fileExtLower, ".qoa") == 0)
    {
        // NOTE: qoaplay_open_memory() keeps its own copy of the provided data
        qoaplay_desc *ctxQoa = qoaplay_open_memory(data, dataSize);

        if (ctxQoa != NULL)
        {
            music.ctxType = MUSIC_AUDIO_QOA;
            music.ctxData = ctxQoa;

            music.stream = LoadAudioStream(ctxQoa->info.samplerate, 32, ctxQoa->info.channels);
            music.frameCount = ctxQoa->info.samples;
            music.looping = true;
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_XM)
    else if (strcmp(fileExtLower, ".xm") == 0)
    {
        jar_xm_context_t *ctxXm = NULL;

        // NOTE: jar_xm keeps its own copy of the module data
        int result = jar_xm_create_context_safe(&ctxXm, (const char *)data, (size_t)dataSize, AUDIO.System.device.sampleRate);

        if ((result == 0) && (ctxXm != NULL))
        {
            music.ctxType = MUSIC_MODULE_XM;
            music.ctxData = ctxXm;

            jar_xm_set_max_loop_count(ctxXm, 0);

            music.stream = LoadAudioStream(AUDIO.System.device.sampleRate, 32, 2);
            music.frameCount = (unsigned int)jar_xm_get_remaining_samples(ctxXm);
            music.looping = true;
            jar_xm_reset(ctxXm);
            musicLoaded = true;
        }
    }
#endif
#if defined(SUPPORT_FILEFORMAT_MOD)
    else if (strcmp(fileExtLower, ".mod") == 0)
    {
        jar_mod_context_t *ctxMod = (jar_mod_context_t *)RL_CALLOC(1, sizeof(jar_mod_context_t));

        if (ctxMod != NULL)
        {
            music.ctxType = MUSIC_MODULE_MOD;
            music.ctxData = ctxMod;

            jar_mod_init(ctxMod);

            // NOTE: jar_mod keeps pointers into the module data, so we own a copy of it,
            // registered on the context so jar_mod_unload() takes care of freeing it
            unsigned char *modData = (unsigned char *)RL_MALLOC((size_t)dataSize);

            if (modData != NULL)
            {
                memcpy(modData, data, (size_t)dataSize);
                ctxMod->modfile = modData;
                ctxMod->modfilesize = (unsigned long)dataSize;

                if (jar_mod_load(ctxMod, (void *)modData, dataSize))
                {
                    music.stream = LoadAudioStream(AUDIO.System.device.sampleRate, 16, 2);
                    music.frameCount = (unsigned int)jar_mod_max_samples(ctxMod);
                    music.looping = true;
                    musicLoaded = true;
                }
            }
        }
    }
#endif
    else TRACELOG(LOG_WARNING, "STREAM: Data format not supported");

    if (!musicLoaded || (music.stream.buffer == NULL) || (music.frameCount == 0))
    {
        UnloadMusicContext(music.ctxType, music.ctxData);
        UnloadAudioStream(music.stream);

        Music empty = { 0 };
        TRACELOG(LOG_WARNING, "STREAM: Music data could not be loaded");
        return empty;
    }

    TRACELOG(LOG_INFO, "STREAM: Music data loaded successfully");
    TRACELOG(LOG_INFO, "    > Sample rate:   %i Hz", music.stream.sampleRate);
    TRACELOG(LOG_INFO, "    > Sample size:   %i bits", music.stream.sampleSize);
    TRACELOG(LOG_INFO, "    > Channels:      %i (%s)", music.stream.channels, (music.stream.channels == 1)? "Mono" : "Stereo");
    TRACELOG(LOG_INFO, "    > Total frames:  %i", music.frameCount);

    return music;
}

// Checks if a music stream is valid (context and buffers initialized)
bool IsMusicValid(Music music)
{
    return ((music.ctxData != NULL) &&          // Validate context loaded
            (music.frameCount > 0) &&           // Validate audio frame count
            (music.stream.buffer != NULL) &&    // Validate stream buffer
            (music.stream.sampleRate > 0) &&    // Validate sample rate
            (music.stream.sampleSize > 0) &&    // Validate sample size
            (music.stream.channels > 0));       // Validate number of channels
}

// Unload music stream
void UnloadMusicStream(Music music)
{
    UnloadAudioStream(music.stream);
    UnloadMusicContext(music.ctxType, music.ctxData);
}

// Start music playing (open stream)
void PlayMusicStream(Music music)
{
    if (music.stream.buffer == NULL) return;

    // For music streams, we need to make sure we maintain the frame cursor position
    // NOTE: PlayAudioStream() resets the cursor position, but the sub-buffers keep
    // the data already decoded, so it must be preserved
    unsigned int frameCursorPos = music.stream.buffer->frameCursorPos;
    PlayAudioStream(music.stream);
    music.stream.buffer->frameCursorPos = frameCursorPos;
}

// Check if any music is playing
bool IsMusicStreamPlaying(Music music)
{
    return IsAudioStreamPlaying(music.stream);
}

// Stop music playing (close stream)
void StopMusicStream(Music music)
{
    if (music.stream.buffer == NULL) return;

    StopAudioStream(music.stream);

    RewindMusicContext(music.ctxType, music.ctxData);
}

// Pause music playing
void PauseMusicStream(Music music)
{
    PauseAudioStream(music.stream);
}

// Resume music playing
void ResumeMusicStream(Music music)
{
    ResumeAudioStream(music.stream);
}

// Seek music to a position (in seconds)
void SeekMusicStream(Music music, float position)
{
    if (!IsMusicValid(music)) return;

    if (position < 0.0f) position = 0.0f;

    unsigned int positionInFrames = (unsigned int)(position*(float)music.stream.sampleRate);
    if (positionInFrames >= music.frameCount) positionInFrames = music.frameCount - 1;

    switch (music.ctxType)
    {
#if defined(SUPPORT_FILEFORMAT_WAV)
        case MUSIC_AUDIO_WAV: drwav_seek_to_pcm_frame((drwav *)music.ctxData, positionInFrames); break;
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
        case MUSIC_AUDIO_OGG: stb_vorbis_seek_frame((stb_vorbis *)music.ctxData, positionInFrames); break;
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
        case MUSIC_AUDIO_FLAC: drflac_seek_to_pcm_frame((drflac *)music.ctxData, positionInFrames); break;
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
        case MUSIC_AUDIO_MP3: drmp3_seek_to_pcm_frame((drmp3 *)music.ctxData, positionInFrames); break;
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
        case MUSIC_AUDIO_QOA:
        {
            // NOTE: QOA seeking granularity is one QOA frame (QOA_FRAME_LEN samples)
            qoaplay_seek_frame((qoaplay_desc *)music.ctxData, (int)(positionInFrames/QOA_FRAME_LEN));
            positionInFrames = ((qoaplay_desc *)music.ctxData)->sample_position;
        } break;
#endif
#if defined(SUPPORT_FILEFORMAT_XM)
        case MUSIC_MODULE_XM:
        {
            // NOTE: jar_xm only supports rewinding to the beginning of the module
            jar_xm_reset((jar_xm_context_t *)music.ctxData);
            positionInFrames = 0;
        } break;
#endif
#if defined(SUPPORT_FILEFORMAT_MOD)
        case MUSIC_MODULE_MOD:
        {
            // NOTE: jar_mod only supports rewinding to the beginning of the module
            jar_mod_seek_start((jar_mod_context_t *)music.ctxData);
            positionInFrames = 0;
        } break;
#endif
        default: break;
    }

    music.stream.buffer->framesProcessed = positionInFrames;
}

// Update (re-fill) music buffers if data already processed
void UpdateMusicStream(Music music)
{
    if (!IsMusicValid(music)) return;

    unsigned int subBufferSizeInFrames = music.stream.buffer->sizeInFrames/2;
    if (subBufferSizeInFrames == 0) return;

    // On first call of this function we lazily pre-allocate a temp buffer to read audio file/memory data in
    unsigned int frameSize = music.stream.channels*music.stream.sampleSize/8;
    size_t pcmSize = (size_t)subBufferSizeInFrames*frameSize;

    if (AUDIO.System.pcmBufferSize < pcmSize)
    {
        RL_FREE(AUDIO.System.pcmBuffer);
        AUDIO.System.pcmBuffer = RL_CALLOC(1, pcmSize);
        AUDIO.System.pcmBufferSize = (AUDIO.System.pcmBuffer != NULL)? pcmSize : 0;
    }

    if (AUDIO.System.pcmBuffer == NULL) return;

    unsigned char *pcm = (unsigned char *)AUDIO.System.pcmBuffer;

    // Check both sub-buffers to check if they require refilling
    for (int i = 0; i < 2; i++)
    {
        if (!music.stream.buffer->isSubBufferProcessed[i]) continue;    // No refilling required, move to next sub-buffer

        unsigned int framesLeft = music.frameCount - music.stream.buffer->framesProcessed;
        unsigned int framesToStream = ((framesLeft >= subBufferSizeInFrames) || music.looping)? subBufferSizeInFrames : framesLeft;

        int frameCountStillNeeded = (int)framesToStream;
        int frameCountReadTotal = 0;
        int stallCount = 0;

        while (frameCountStillNeeded > 0)
        {
            int frameCountRead = 0;
            void *dst = pcm + (size_t)frameCountReadTotal*frameSize;

            switch (music.ctxType)
            {
#if defined(SUPPORT_FILEFORMAT_WAV)
                case MUSIC_AUDIO_WAV:
                {
                    if (music.stream.sampleSize == 16) frameCountRead = (int)drwav_read_pcm_frames_s16((drwav *)music.ctxData, (drwav_uint64)frameCountStillNeeded, (short *)dst);
                    else if (music.stream.sampleSize == 32) frameCountRead = (int)drwav_read_pcm_frames_f32((drwav *)music.ctxData, (drwav_uint64)frameCountStillNeeded, (float *)dst);
                    else frameCountRead = (int)drwav_read_pcm_frames((drwav *)music.ctxData, (drwav_uint64)frameCountStillNeeded, dst);
                } break;
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
                case MUSIC_AUDIO_OGG:
                {
                    frameCountRead = stb_vorbis_get_samples_short_interleaved((stb_vorbis *)music.ctxData, (int)music.stream.channels, (short *)dst, frameCountStillNeeded*(int)music.stream.channels);
                } break;
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
                case MUSIC_AUDIO_FLAC:
                {
                    frameCountRead = (int)drflac_read_pcm_frames_s16((drflac *)music.ctxData, (drflac_uint64)frameCountStillNeeded, (short *)dst);
                } break;
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
                case MUSIC_AUDIO_MP3:
                {
                    frameCountRead = (int)drmp3_read_pcm_frames_f32((drmp3 *)music.ctxData, (drmp3_uint64)frameCountStillNeeded, (float *)dst);
                } break;
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
                case MUSIC_AUDIO_QOA:
                {
                    frameCountRead = (int)qoaplay_decode((qoaplay_desc *)music.ctxData, (float *)dst, frameCountStillNeeded);
                } break;
#endif
#if defined(SUPPORT_FILEFORMAT_XM)
                case MUSIC_MODULE_XM:
                {
                    // NOTE: Internally XM generation is always 2 channels (stereo), 32bit float
                    jar_xm_generate_samples((jar_xm_context_t *)music.ctxData, (float *)dst, (size_t)frameCountStillNeeded);
                    frameCountRead = frameCountStillNeeded;
                } break;
#endif
#if defined(SUPPORT_FILEFORMAT_MOD)
                case MUSIC_MODULE_MOD:
                {
                    // NOTE: 3rd parameter is the number of stereo 16bit frames required
                    jar_mod_fillbuffer((jar_mod_context_t *)music.ctxData, (short *)dst, (unsigned long)frameCountStillNeeded, 0);
                    frameCountRead = frameCountStillNeeded;
                } break;
#endif
                default: break;
            }

            if (frameCountRead > 0)
            {
                if (frameCountRead > frameCountStillNeeded) frameCountRead = frameCountStillNeeded;

                frameCountReadTotal += frameCountRead;
                frameCountStillNeeded -= frameCountRead;
                stallCount = 0;
            }

            if (frameCountStillNeeded == 0) break;

            // Not enough data available: rewind the decoder to loop over the music data
            // NOTE: stallCount avoids an infinite loop if the context can not provide any data
            if (++stallCount > 2) break;

            RewindMusicContext(music.ctxType, music.ctxData);
        }

        // Any missing frame is filled with silence, streaming must always provide a full sub-buffer
        if (frameCountStillNeeded > 0) memset(pcm + (size_t)frameCountReadTotal*frameSize, 0, (size_t)frameCountStillNeeded*frameSize);

        UpdateAudioStream(music.stream, pcm, (int)framesToStream);

        // NOTE: UpdateAudioStream() advances framesProcessed by a full sub-buffer,
        // music time tracking requires it wrapped to the music length
        music.stream.buffer->framesProcessed = music.stream.buffer->framesProcessed%music.frameCount;

        if (framesLeft <= subBufferSizeInFrames)
        {
            if (!music.looping)
            {
                // Streaming is ending, we filled latest frames from input
                StopMusicStream(music);
                return;
            }
        }
    }
}

// Set volume for music (1.0 is max level)
void SetMusicVolume(Music music, float volume)
{
    SetAudioStreamVolume(music.stream, volume);
}

// Set pitch for music (1.0 is base level)
void SetMusicPitch(Music music, float pitch)
{
    SetAudioStreamPitch(music.stream, pitch);
}

// Set pan for a music (-1.0 left, 0.0 center, 1.0 right)
void SetMusicPan(Music music, float pan)
{
    SetAudioStreamPan(music.stream, pan);
}

// Get music time length (in seconds)
float GetMusicTimeLength(Music music)
{
    if (!IsMusicValid(music)) return 0.0f;

    return (float)music.frameCount/(float)music.stream.sampleRate;
}

// Get current music time played (in seconds)
float GetMusicTimePlayed(Music music)
{
    if (!IsMusicValid(music)) return 0.0f;

    float secondsPlayed = 0.0f;

#if defined(SUPPORT_FILEFORMAT_XM)
    if (music.ctxType == MUSIC_MODULE_XM)
    {
        uint64_t framesPlayed = 0;
        jar_xm_get_position((jar_xm_context_t *)music.ctxData, NULL, NULL, NULL, &framesPlayed);
        secondsPlayed = (float)framesPlayed/(float)music.stream.sampleRate;

        return secondsPlayed;
    }
#endif

    int subBufferSize = (int)(music.stream.buffer->sizeInFrames/2);
    if (subBufferSize <= 0) return 0.0f;

    int framesProcessed = (int)music.stream.buffer->framesProcessed;
    int framesInFirstBuffer = music.stream.buffer->isSubBufferProcessed[0]? 0 : subBufferSize;
    int framesInSecondBuffer = music.stream.buffer->isSubBufferProcessed[1]? 0 : subBufferSize;
    int framesSentToMix = (int)(music.stream.buffer->frameCursorPos%(unsigned int)subBufferSize);

    int framesPlayed = (framesProcessed - framesInFirstBuffer - framesInSecondBuffer + framesSentToMix)%(int)music.frameCount;
    if (framesPlayed < 0) framesPlayed += (int)music.frameCount;

    secondsPlayed = (float)framesPlayed/(float)music.stream.sampleRate;

    return secondsPlayed;
}

//----------------------------------------------------------------------------------
// Module Functions Definition - AudioStream management
//----------------------------------------------------------------------------------

// Load audio stream (to stream raw audio pcm data)
AudioStream LoadAudioStream(unsigned int sampleRate, unsigned int sampleSize, unsigned int channels)
{
    AudioStream stream = { 0 };

    if ((sampleRate == 0) || (channels == 0) || !IsSampleSizeValid(sampleSize))
    {
        TRACELOG(LOG_WARNING, "STREAM: Invalid format requested (%i Hz, %i bit, %i channels)", sampleRate, sampleSize, channels);
        return stream;
    }

    if (!AUDIO.System.isReady)
    {
        TRACELOG(LOG_WARNING, "STREAM: Audio device not initialized");
        return stream;
    }

    stream.sampleRate = sampleRate;
    stream.sampleSize = sampleSize;
    stream.channels = channels;

    ma_format formatIn = GetMiniaudioFormat(sampleSize);

    // The size of a streaming buffer must be at least double the size of a period
    unsigned int periodSize = AUDIO.System.device.playback.internalPeriodSizeInFrames;

    // If the buffer is not set, compute one that would give us a buffer good enough for a decent frame rate
    unsigned int subBufferSize = (AUDIO.Buffer.defaultSize == 0)? (AUDIO.System.device.sampleRate/DEFAULT_AUDIO_BUFFER_SIZE_DIVISOR) : (unsigned int)AUDIO.Buffer.defaultSize;

    if (subBufferSize < periodSize) subBufferSize = periodSize;
    if (subBufferSize == 0) subBufferSize = 1024;

    // Create a double buffer
    AudioBuffer *audioBuffer = LoadAudioBuffer(formatIn, channels, sampleRate, subBufferSize*2, AUDIO_BUFFER_USAGE_STREAM);
    if (audioBuffer == NULL)
    {
        TRACELOG(LOG_WARNING, "STREAM: Failed to load audio buffer, stream could not be created");

        AudioStream empty = { 0 };
        return empty;
    }

    audioBuffer->looping = true;    // Always loop for streaming buffers
    stream.buffer = audioBuffer;

    TRACELOG(LOG_INFO, "STREAM: Initialized successfully (%i Hz, %i bit, %s)", stream.sampleRate, stream.sampleSize, (stream.channels == 1)? "Mono" : "Stereo");

    return stream;
}

// Checks if an audio stream is valid (buffers initialized)
bool IsAudioStreamValid(AudioStream stream)
{
    return ((stream.buffer != NULL) &&      // Validate stream buffer
            (stream.sampleRate > 0) &&      // Validate sample rate
            (stream.sampleSize > 0) &&      // Validate sample size
            (stream.channels > 0));         // Validate number of channels
}

// Unload audio stream and free memory
void UnloadAudioStream(AudioStream stream)
{
    if (stream.buffer == NULL) return;

    // Unload the processors chain attached to the stream, if any
    AudioLock();
    {
        rAudioProcessor *processor = stream.buffer->processor;
        while (processor != NULL)
        {
            rAudioProcessor *next = processor->next;
            RL_FREE(processor);
            processor = next;
        }

        stream.buffer->processor = NULL;
    }
    AudioUnlock();

    UnloadAudioBuffer(stream.buffer);

    TRACELOG(LOG_INFO, "STREAM: Unloaded audio stream data from RAM");
}

// Update audio stream buffers with data
// NOTE 1: Only updates one buffer of the stream source: dequeue -> update -> queue
// NOTE 2: To dequeue a buffer it needs to be processed: IsAudioStreamProcessed()
void UpdateAudioStream(AudioStream stream, const void *data, int frameCount)
{
    if ((stream.buffer == NULL) || (data == NULL) || (frameCount <= 0)) return;

    if (!stream.buffer->isSubBufferProcessed[0] && !stream.buffer->isSubBufferProcessed[1])
    {
        TRACELOG(LOG_WARNING, "STREAM: Buffer not available for updating");
        return;
    }

    ma_uint32 subBufferToUpdate = 0;

    if (stream.buffer->isSubBufferProcessed[0] && stream.buffer->isSubBufferProcessed[1])
    {
        // Both buffers are available for updating
        // Update the first one and make sure the cursor is moved back to the front
        subBufferToUpdate = 0;
        stream.buffer->frameCursorPos = 0;
    }
    else subBufferToUpdate = (stream.buffer->isSubBufferProcessed[0])? 0 : 1;    // Just update whichever sub-buffer is processed

    ma_uint32 subBufferSizeInFrames = stream.buffer->sizeInFrames/2;

    if (subBufferSizeInFrames < (ma_uint32)frameCount)
    {
        TRACELOG(LOG_WARNING, "STREAM: Attempting to write too many frames to buffer");
        return;
    }

    const ma_uint32 frameSize = stream.channels*(stream.sampleSize/8);
    unsigned char *subBuffer = stream.buffer->data + (subBufferSizeInFrames*frameSize*subBufferToUpdate);

    // Total frames processed in buffer is always the complete size, filled with 0 if required
    stream.buffer->framesProcessed += subBufferSizeInFrames;

    ma_uint32 bytesToWrite = (ma_uint32)frameCount*frameSize;
    memcpy(subBuffer, data, bytesToWrite);

    // Any leftover frames should be filled with zeros
    ma_uint32 leftoverFrameCount = subBufferSizeInFrames - (ma_uint32)frameCount;
    if (leftoverFrameCount > 0) memset(subBuffer + bytesToWrite, 0, leftoverFrameCount*frameSize);

    stream.buffer->isSubBufferProcessed[subBufferToUpdate] = false;
}

// Check if any audio stream buffers requires refill
bool IsAudioStreamProcessed(AudioStream stream)
{
    if (stream.buffer == NULL) return false;

    return (stream.buffer->isSubBufferProcessed[0] || stream.buffer->isSubBufferProcessed[1]);
}

// Play audio stream
void PlayAudioStream(AudioStream stream)
{
    PlayAudioBuffer(stream.buffer);
}

// Pause audio stream
void PauseAudioStream(AudioStream stream)
{
    PauseAudioBuffer(stream.buffer);
}

// Resume audio stream playing
void ResumeAudioStream(AudioStream stream)
{
    ResumeAudioBuffer(stream.buffer);
}

// Check if audio stream is playing
bool IsAudioStreamPlaying(AudioStream stream)
{
    return IsAudioBufferPlaying(stream.buffer);
}

// Stop audio stream
void StopAudioStream(AudioStream stream)
{
    StopAudioBuffer(stream.buffer);
}

// Set volume for audio stream (1.0 is max level)
void SetAudioStreamVolume(AudioStream stream, float volume)
{
    SetAudioBufferVolume(stream.buffer, volume);
}

// Set pitch for audio stream (1.0 is base level)
void SetAudioStreamPitch(AudioStream stream, float pitch)
{
    SetAudioBufferPitch(stream.buffer, pitch);
}

// Set pan for audio stream (-1.0 left, 0.0 center, 1.0 right)
void SetAudioStreamPan(AudioStream stream, float pan)
{
    SetAudioBufferPan(stream.buffer, (1.0f - ClampFloat(pan, -1.0f, 1.0f))*0.5f);
}

// Default size for new audio streams
void SetAudioStreamBufferSizeDefault(int size)
{
    if (size < 0) size = 0;

    AUDIO.Buffer.defaultSize = size;
}

// Audio thread callback to request new data
void SetAudioStreamCallback(AudioStream stream, AudioCallback callback)
{
    if (stream.buffer == NULL) return;

    AudioLock();
    {
        stream.buffer->callback = callback;
    }
    AudioUnlock();
}

// Attach audio stream processor to stream
// NOTE: Processor receives frames x 2 samples as 'float' (stereo)
void AttachAudioStreamProcessor(AudioStream stream, AudioCallback process)
{
    if ((stream.buffer == NULL) || (process == NULL)) return;

    AudioLock();
    {
        AttachProcessor(&stream.buffer->processor, process);
    }
    AudioUnlock();
}

// Detach audio stream processor from stream
void DetachAudioStreamProcessor(AudioStream stream, AudioCallback process)
{
    if ((stream.buffer == NULL) || (process == NULL)) return;

    AudioLock();
    {
        DetachProcessor(&stream.buffer->processor, process);
    }
    AudioUnlock();
}

// Attach audio stream processor to the entire audio pipeline
// NOTE: Processor receives frames x 2 samples as 'float' (stereo)
void AttachAudioMixedProcessor(AudioCallback process)
{
    if ((process == NULL) || !AUDIO.System.isReady) return;

    AudioLock();
    {
        AttachProcessor(&AUDIO.mixedProcessor, process);
    }
    AudioUnlock();
}

// Detach audio stream processor from the entire audio pipeline
void DetachAudioMixedProcessor(AudioCallback process)
{
    if ((process == NULL) || !AUDIO.System.isReady) return;

    AudioLock();
    {
        DetachProcessor(&AUDIO.mixedProcessor, process);
    }
    AudioUnlock();
}

//----------------------------------------------------------------------------------
// Module specific Functions Definition - Audio reading and mixing
//----------------------------------------------------------------------------------

// Log callback function, redirects miniaudio messages to raylib TRACELOG
static void OnLog(void *pUserData, ma_uint32 level, const char *pMessage)
{
    (void)pUserData;
    (void)level;

    TRACELOG(LOG_WARNING, "miniaudio: %s", pMessage);   // All log messages from miniaudio are errors
}

// Reading audio buffer data in the internal (source) format
// NOTE: Returns the number of frames actually read
static ma_uint32 ReadAudioBufferFramesInInternalFormat(AudioBuffer *audioBuffer, void *framesOut, ma_uint32 frameCount)
{
    // Using audio buffer callback: the user fills the frames directly
    if (audioBuffer->callback != NULL)
    {
        audioBuffer->callback(framesOut, frameCount);
        audioBuffer->framesProcessed += frameCount;

        return frameCount;
    }

    if ((audioBuffer->data == NULL) || (audioBuffer->sizeInFrames == 0)) return 0;

    ma_uint32 subBufferSizeInFrames = (audioBuffer->sizeInFrames > 1)? (audioBuffer->sizeInFrames/2) : audioBuffer->sizeInFrames;
    ma_uint32 currentSubBufferIndex = audioBuffer->frameCursorPos/subBufferSizeInFrames;

    if (currentSubBufferIndex > 1) return 0;

    // Another thread can update the processed state of buffers, so
    // we just take a copy here to try and avoid potential synchronization problems
    bool isSubBufferProcessed[2] = { audioBuffer->isSubBufferProcessed[0], audioBuffer->isSubBufferProcessed[1] };

    ma_uint32 frameSizeInBytes = ma_get_bytes_per_frame(audioBuffer->converter.formatIn, audioBuffer->converter.channelsIn);

    // Fill out every frame until we find a buffer that's marked as processed, then fill the remainder with 0
    ma_uint32 framesRead = 0;
    while (true)
    {
        // We break from this loop differently depending on the buffer's usage:
        //  - for static buffers we simply fill as much data as we can
        //  - for streaming buffers we only fill the halves that are processed,
        //    unprocessed halves must keep their audio data intact
        if (audioBuffer->usage == AUDIO_BUFFER_USAGE_STATIC)
        {
            if (framesRead >= frameCount) break;
        }
        else if (isSubBufferProcessed[currentSubBufferIndex]) break;

        ma_uint32 totalFramesRemaining = (frameCount - framesRead);
        if (totalFramesRemaining == 0) break;

        ma_uint32 framesRemainingInOutputBuffer = 0;
        if (audioBuffer->usage == AUDIO_BUFFER_USAGE_STATIC) framesRemainingInOutputBuffer = audioBuffer->sizeInFrames - audioBuffer->frameCursorPos;
        else
        {
            ma_uint32 firstFrameIndexOfThisSubBuffer = subBufferSizeInFrames*currentSubBufferIndex;
            framesRemainingInOutputBuffer = subBufferSizeInFrames - (audioBuffer->frameCursorPos - firstFrameIndexOfThisSubBuffer);
        }

        if (framesRemainingInOutputBuffer == 0) break;

        ma_uint32 framesToRead = totalFramesRemaining;
        if (framesToRead > framesRemainingInOutputBuffer) framesToRead = framesRemainingInOutputBuffer;

        memcpy((unsigned char *)framesOut + (framesRead*frameSizeInBytes), audioBuffer->data + (audioBuffer->frameCursorPos*frameSizeInBytes), framesToRead*frameSizeInBytes);
        audioBuffer->frameCursorPos = (audioBuffer->frameCursorPos + framesToRead)%audioBuffer->sizeInFrames;
        framesRead += framesToRead;

        // If we've read to the end of the buffer, mark it as processed
        if (framesToRead == framesRemainingInOutputBuffer)
        {
            audioBuffer->isSubBufferProcessed[currentSubBufferIndex] = true;
            isSubBufferProcessed[currentSubBufferIndex] = true;

            currentSubBufferIndex = (currentSubBufferIndex + 1)%2;

            // We need to break from this loop if we're not looping
            if (!audioBuffer->looping)
            {
                StopAudioBuffer(audioBuffer);
                break;
            }
        }
    }

    // Zero-fill excess
    ma_uint32 totalFramesRemaining = (frameCount - framesRead);
    if (totalFramesRemaining > 0)
    {
        memset((unsigned char *)framesOut + (framesRead*frameSizeInBytes), 0, totalFramesRemaining*frameSizeInBytes);

        // For static buffers we can fill the remaining frames with silence for safety, but we don't want
        // to report those frames as read: the caller uses the return value to know whether a
        // non-looping sound has finished playback
        if (audioBuffer->usage != AUDIO_BUFFER_USAGE_STATIC) framesRead += totalFramesRemaining;
    }

    return framesRead;
}

// Reading audio buffer data, converted to the mixing format (f32, device channels/rate)
static ma_uint32 ReadAudioBufferFramesInMixingFormat(AudioBuffer *audioBuffer, float *framesOut, ma_uint32 frameCount)
{
    // We continuously convert data from the AudioBuffer internal format to the mixing format until
    // frameCount frames have been output; we never read more input data than strictly required,
    // that is what ma_data_converter_get_required_input_frame_count() is for
    ma_uint8 inputBuffer[4096] = { 0 };
    ma_uint32 inputBufferFrameCap = (ma_uint32)(sizeof(inputBuffer)/ma_get_bytes_per_frame(audioBuffer->converter.formatIn, audioBuffer->converter.channelsIn));

    if (inputBufferFrameCap == 0) return 0;

    ma_uint32 totalOutputFramesProcessed = 0;
    while (totalOutputFramesProcessed < frameCount)
    {
        ma_uint64 outputFramesToProcessThisIteration = frameCount - totalOutputFramesProcessed;
        ma_uint64 inputFramesToProcessThisIteration = 0;

        ma_data_converter_get_required_input_frame_count(&audioBuffer->converter, outputFramesToProcessThisIteration, &inputFramesToProcessThisIteration);
        if (inputFramesToProcessThisIteration > inputBufferFrameCap) inputFramesToProcessThisIteration = inputBufferFrameCap;

        float *runningFramesOut = framesOut + (totalOutputFramesProcessed*audioBuffer->converter.channelsOut);

        // At this point we can convert the data to our mixing format
        ma_uint64 inputFramesProcessedThisIteration = ReadAudioBufferFramesInInternalFormat(audioBuffer, inputBuffer, (ma_uint32)inputFramesToProcessThisIteration);
        ma_uint64 outputFramesProcessedThisIteration = outputFramesToProcessThisIteration;
        ma_data_converter_process_pcm_frames(&audioBuffer->converter, inputBuffer, &inputFramesProcessedThisIteration, runningFramesOut, &outputFramesProcessedThisIteration);

        totalOutputFramesProcessed += (ma_uint32)outputFramesProcessedThisIteration;

        if (inputFramesProcessedThisIteration < inputFramesToProcessThisIteration) break;    // Ran out of input data

        // This should never be hit, but added for safety: get out of the loop when nothing is processed
        if ((inputFramesProcessedThisIteration == 0) && (outputFramesProcessedThisIteration == 0)) break;
    }

    return totalOutputFramesProcessed;
}

// Sending audio data to device callback, the actual mixer
// NOTE: All the audio buffers on the list are mixed into pFramesOut
static void OnSendAudioDataToDevice(ma_device *pDevice, void *pFramesOut, const void *pFramesInput, ma_uint32 frameCount)
{
    (void)pFramesInput;

    const ma_uint32 deviceChannels = pDevice->playback.channels;

    // Mixing is basically just an accumulation, we need to initialize the output buffer to 0
    memset(pFramesOut, 0, frameCount*deviceChannels*ma_get_bytes_per_sample(pDevice->playback.format));

    // Using a mutex here for thread-safety which makes things not real-time
    ma_mutex_lock(&AUDIO.System.lock);
    {
        for (AudioBuffer *audioBuffer = AUDIO.Buffer.first; audioBuffer != NULL; audioBuffer = audioBuffer->next)
        {
            // Ignore stopped or paused sounds
            if (!audioBuffer->playing || audioBuffer->paused) continue;

            ma_uint32 framesRead = 0;
            while (framesRead < frameCount)
            {
                float tempBuffer[1024] = { 0 };     // Frames for stereo
                const ma_uint32 tempBufferFrameCap = (ma_uint32)(sizeof(tempBuffer)/sizeof(tempBuffer[0])/AUDIO_DEVICE_CHANNELS);

                ma_uint32 framesToReadRightNow = frameCount - framesRead;
                if (framesToReadRightNow > tempBufferFrameCap) framesToReadRightNow = tempBufferFrameCap;

                ma_uint32 framesJustRead = ReadAudioBufferFramesInMixingFormat(audioBuffer, tempBuffer, framesToReadRightNow);

                if (framesJustRead > 0)
                {
                    float *framesOut = (float *)pFramesOut + (framesRead*deviceChannels);

                    // Apply the processors chain attached to this buffer
                    for (rAudioProcessor *processor = audioBuffer->processor; processor != NULL; processor = processor->next)
                    {
                        if (processor->process != NULL) processor->process(tempBuffer, framesJustRead);
                    }

                    MixAudioFrames(framesOut, tempBuffer, framesJustRead, audioBuffer);

                    framesRead += framesJustRead;
                }

                if (!audioBuffer->playing) break;

                // Not able to read all requested frames: either loop around or stop the buffer
                if (framesJustRead < framesToReadRightNow)
                {
                    if (!audioBuffer->looping)
                    {
                        StopAudioBuffer(audioBuffer);
                        break;
                    }

                    // Move the cursor position back to the start and continue mixing
                    audioBuffer->frameCursorPos = 0;

                    if (framesJustRead == 0) break;     // No progress possible, avoid an infinite loop
                }
            }
        }

        // Apply the processors chain attached to the final mix
        for (rAudioProcessor *processor = AUDIO.mixedProcessor; processor != NULL; processor = processor->next)
        {
            if (processor->process != NULL) processor->process(pFramesOut, frameCount);
        }
    }
    ma_mutex_unlock(&AUDIO.System.lock);
}

// Mix audio frames applying the buffer volume and pan
// NOTE: framesIn/framesOut are in the mixing format (f32, device channels)
static void MixAudioFrames(float *framesOut, const float *framesIn, ma_uint32 frameCount, AudioBuffer *buffer)
{
    const float localVolume = buffer->volume;
    const ma_uint32 channels = AUDIO.System.device.playback.channels;

    if (channels == 2)  // We consider panning
    {
        const float left = buffer->pan;
        const float right = 1.0f - left;

        // Fast sine approximation in [0..1] for pan law: y = 0.5f*x*(3 - x*x)
        const float levels[2] = { localVolume*0.5f*left*(3.0f - left*left), localVolume*0.5f*right*(3.0f - right*right) };

        float *frameOut = framesOut;
        const float *frameIn = framesIn;

        for (ma_uint32 frame = 0; frame < frameCount; frame++)
        {
            frameOut[0] += (frameIn[0]*levels[0]);
            frameOut[1] += (frameIn[1]*levels[1]);

            frameOut += 2;
            frameIn += 2;
        }
    }
    else    // Pan is ignored for non-stereo sources
    {
        for (ma_uint32 frame = 0; frame < frameCount; frame++)
        {
            for (ma_uint32 c = 0; c < channels; c++) framesOut[frame*channels + c] += (framesIn[frame*channels + c]*localVolume);
        }
    }
}

//----------------------------------------------------------------------------------
// Module specific Functions Definition - Helpers
//----------------------------------------------------------------------------------

// Append a processor at the end of a processors list
// NOTE: Caller must hold the audio lock
static void AttachProcessor(rAudioProcessor **list, AudioCallback process)
{
    rAudioProcessor *processor = (rAudioProcessor *)RL_CALLOC(1, sizeof(rAudioProcessor));
    if (processor == NULL) return;

    processor->process = process;

    rAudioProcessor *last = *list;
    while ((last != NULL) && (last->next != NULL)) last = last->next;

    if (last == NULL) *list = processor;
    else
    {
        processor->prev = last;
        last->next = processor;
    }
}

// Remove the first processor matching the callback from a processors list
// NOTE: Caller must hold the audio lock
static void DetachProcessor(rAudioProcessor **list, AudioCallback process)
{
    for (rAudioProcessor *processor = *list; processor != NULL; processor = processor->next)
    {
        if (processor->process != process) continue;

        if (processor->prev == NULL) *list = processor->next;
        else processor->prev->next = processor->next;

        if (processor->next != NULL) processor->next->prev = processor->prev;

        RL_FREE(processor);
        return;
    }
}

// Get the miniaudio format matching a raylib sample size, pre: sampleSize is 8, 16 or 32
static ma_format GetMiniaudioFormat(unsigned int sampleSize)
{
    if (sampleSize == 8) return ma_format_u8;
    if (sampleSize == 16) return ma_format_s16;

    return ma_format_f32;
}

// Check if a sample size (bit depth) is supported, 24 bit is not supported
static bool IsSampleSizeValid(unsigned int sampleSize)
{
    return ((sampleSize == 8) || (sampleSize == 16) || (sampleSize == 32));
}

// Copy a file extension to a lowercase NULL-terminated buffer, pre: out != NULL, outSize > 0
static void GetFileExtLower(char *out, size_t outSize, const char *fileType)
{
    out[0] = '\0';
    if (fileType == NULL) return;

    size_t i = 0;
    for (; (i < (outSize - 1)) && (fileType[i] != '\0'); i++)
    {
        char c = fileType[i];
        if ((c >= 'A') && (c <= 'Z')) c = (char)(c + 32);
        out[i] = c;
    }

    out[i] = '\0';
}

// Lock the audio mixing lock, no-op if the audio device is not initialized
static void AudioLock(void)
{
    if (AUDIO.System.isReady) ma_mutex_lock(&AUDIO.System.lock);
}

// Unlock the audio mixing lock, no-op if the audio device is not initialized
static void AudioUnlock(void)
{
    if (AUDIO.System.isReady) ma_mutex_unlock(&AUDIO.System.lock);
}

// Clamp a float value to a range, pre: min <= max
static float ClampFloat(float value, float min, float max)
{
    if (value < min) return min;
    if (value > max) return max;

    return value;
}

// Rewind a music decoding context to the first frame, no-op if the context is not loaded
static void RewindMusicContext(int ctxType, void *ctxData)
{
    if (ctxData == NULL) return;

    switch (ctxType)
    {
#if defined(SUPPORT_FILEFORMAT_WAV)
        case MUSIC_AUDIO_WAV: drwav_seek_to_pcm_frame((drwav *)ctxData, 0); break;
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
        case MUSIC_AUDIO_OGG: stb_vorbis_seek_start((stb_vorbis *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
        case MUSIC_AUDIO_FLAC: drflac_seek_to_pcm_frame((drflac *)ctxData, 0); break;
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
        case MUSIC_AUDIO_MP3: drmp3_seek_to_pcm_frame((drmp3 *)ctxData, 0); break;
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
        case MUSIC_AUDIO_QOA: qoaplay_rewind((qoaplay_desc *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_XM)
        case MUSIC_MODULE_XM: jar_xm_reset((jar_xm_context_t *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_MOD)
        case MUSIC_MODULE_MOD: jar_mod_seek_start((jar_mod_context_t *)ctxData); break;
#endif
        default: break;
    }
}

// Unload a music decoding context, no-op if the context is not loaded
static void UnloadMusicContext(int ctxType, void *ctxData)
{
    if (ctxData == NULL) return;

    switch (ctxType)
    {
#if defined(SUPPORT_FILEFORMAT_WAV)
        case MUSIC_AUDIO_WAV:
        {
            drwav_uninit((drwav *)ctxData);
            RL_FREE(ctxData);
        } break;
#endif
#if defined(SUPPORT_FILEFORMAT_OGG)
        case MUSIC_AUDIO_OGG: stb_vorbis_close((stb_vorbis *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_FLAC)
        case MUSIC_AUDIO_FLAC: drflac_close((drflac *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_MP3)
        case MUSIC_AUDIO_MP3:
        {
            drmp3_uninit((drmp3 *)ctxData);
            RL_FREE(ctxData);
        } break;
#endif
#if defined(SUPPORT_FILEFORMAT_QOA)
        case MUSIC_AUDIO_QOA: qoaplay_close((qoaplay_desc *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_XM)
        case MUSIC_MODULE_XM: jar_xm_free_context((jar_xm_context_t *)ctxData); break;
#endif
#if defined(SUPPORT_FILEFORMAT_MOD)
        case MUSIC_MODULE_MOD:
        {
            jar_mod_unload((jar_mod_context_t *)ctxData);
            RL_FREE(ctxData);
        } break;
#endif
        default: break;
    }
}

#endif      // SUPPORT_MODULE_RAUDIO
