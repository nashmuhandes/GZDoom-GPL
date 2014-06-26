/*
** oalsound.cpp
** System interface for sound; uses OpenAL
**
**---------------------------------------------------------------------------
** Copyright 2008-2010 Chris Robinson
** All rights reserved.
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
** 1. Redistributions of source code must retain the above copyright
**    notice, this list of conditions and the following disclaimer.
** 2. Redistributions in binary form must reproduce the above copyright
**    notice, this list of conditions and the following disclaimer in the
**    documentation and/or other materials provided with the distribution.
** 3. The name of the author may not be used to endorse or promote products
**    derived from this software without specific prior written permission.
**
** THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
** IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
** OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
** IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
** INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
** NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
** DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
** THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
** (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
** THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
**---------------------------------------------------------------------------
**
*/

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define USE_WINDOWS_DWORD
#endif

#include "doomstat.h"
#include "templates.h"
#include "oalsound.h"
#include "c_cvars.h"
#include "c_dispatch.h"
#include "i_system.h"
#include "v_text.h"
#include "gi.h"
#include "actor.h"
#include "r_state.h"
#include "w_wad.h"
#include "i_music.h"
#include "i_musicinterns.h"
#include "tempfiles.h"


CVAR (String, snd_aldevice, "Default", CVAR_ARCHIVE|CVAR_GLOBALCONFIG)
CVAR (Bool, snd_efx, true, CVAR_ARCHIVE|CVAR_GLOBALCONFIG)


void I_BuildALDeviceList(FOptionValues *opt)
{
    opt->mValues.Resize(1);
    opt->mValues[0].TextValue = "Default";
    opt->mValues[0].Text = "Default";

#ifndef NO_OPENAL
    const ALCchar *names = (alcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT") ?
                            alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER) :
                            alcGetString(NULL, ALC_DEVICE_SPECIFIER));
    if(!names)
        Printf("Failed to get device list: %s\n", alcGetString(NULL, alcGetError(NULL)));
    else while(*names)
    {
        unsigned int i = opt->mValues.Reserve(1);
        opt->mValues[i].TextValue = names;
        opt->mValues[i].Text = names;

        names += strlen(names)+1;
    }
#endif
}

#ifndef NO_OPENAL

#include <algorithm>
#include <memory>
#include <string>
#include <vector>


EXTERN_CVAR (Int, snd_channels)
EXTERN_CVAR (Int, snd_samplerate)
EXTERN_CVAR (Bool, snd_waterreverb)
EXTERN_CVAR (Bool, snd_pitched)


#define MAKE_PTRID(x)  ((void*)(uintptr_t)(x))
#define GET_PTRID(x)  ((uint32)(uintptr_t)(x))

#define foreach(type, name, vec) \
    for(std::vector<type>::iterator (name) = (vec).begin(), \
        (_end_##name) = (vec).end(); \
        (name) != (_end_##name);(name)++)


static ALenum checkALError(const char *fn, unsigned int ln)
{
    ALenum err = alGetError();
    if(err != AL_NO_ERROR)
    {
        if(strchr(fn, '/'))
            fn = strrchr(fn, '/')+1;
        else if(strchr(fn, '\\'))
            fn = strrchr(fn, '\\')+1;
        Printf(">>>>>>>>>>>> Received AL error %s (%#x), %s:%u\n", alGetString(err), err, fn, ln);
    }
    return err;
}
#define getALError() checkALError(__FILE__, __LINE__)

static ALCenum checkALCError(ALCdevice *device, const char *fn, unsigned int ln)
{
    ALCenum err = alcGetError(device);
    if(err != ALC_NO_ERROR)
    {
        if(strchr(fn, '/'))
            fn = strrchr(fn, '/')+1;
        else if(strchr(fn, '\\'))
            fn = strrchr(fn, '\\')+1;
        Printf(">>>>>>>>>>>> Received ALC error %s (%#x), %s:%u\n", alcGetString(device, err), err, fn, ln);
    }
    return err;
}
#define getALCError(d) checkALCError((d), __FILE__, __LINE__)


// Fallback methods for when AL_SOFT_deferred_updates isn't available. In most
// cases these don't actually do anything, except on some Creative drivers
// where they act as appropriate fallbacks.
static ALvoid AL_APIENTRY _wrap_DeferUpdatesSOFT(void)
{
    alcSuspendContext(alcGetCurrentContext());
}

static ALvoid AL_APIENTRY _wrap_ProcessUpdatesSOFT(void)
{
    alcProcessContext(alcGetCurrentContext());
}


class OpenALSoundStream : public SoundStream
{
    OpenALSoundRenderer *Renderer;

    SoundStreamCallback Callback;
    void *UserData;

    std::vector<ALubyte> Data;

    ALsizei SampleRate;
    ALenum Format;
    ALsizei FrameSize;

    static const int BufferCount = 4;
    ALuint Buffers[BufferCount];
    ALuint Source;

    bool Playing;
    bool Looping;
    ALfloat Volume;


    std::auto_ptr<FileReader> Reader;
    std::vector<BYTE> DecoderData;
    std::auto_ptr<SoundDecoder> Decoder;
    static bool DecoderCallback(SoundStream *_sstream, void *ptr, int length, void *user)
    {
        OpenALSoundStream *self = static_cast<OpenALSoundStream*>(_sstream);
        if(length < 0) return false;

        size_t got = self->Decoder->read((char*)ptr, length);
        if(got < (unsigned int)length)
        {
            if(!self->Looping || !self->Decoder->seek(0))
                return false;
            got += self->Decoder->read((char*)ptr+got, length-got);
        }

        return (got == (unsigned int)length);
    }


    bool SetupSource()
    {
        /* Get a source, killing the farthest, lowest-priority sound if needed */
        if(Renderer->FreeSfx.size() == 0)
        {
            FSoundChan *lowest = Renderer->FindLowestChannel();
            if(lowest) Renderer->StopChannel(lowest);

            if(Renderer->FreeSfx.size() == 0)
                return false;
        }
        Source = Renderer->FreeSfx.back();
        Renderer->FreeSfx.pop_back();

        /* Set the default properties for localized playback */
        alSource3f(Source, AL_DIRECTION, 0.f, 0.f, 0.f);
        alSource3f(Source, AL_VELOCITY, 0.f, 0.f, 0.f);
        alSource3f(Source, AL_POSITION, 0.f, 0.f, 0.f);
        alSourcef(Source, AL_MAX_GAIN, 1.f);
        alSourcef(Source, AL_GAIN, 1.f);
        alSourcef(Source, AL_PITCH, 1.f);
        alSourcef(Source, AL_ROLLOFF_FACTOR, 0.f);
        alSourcef(Source, AL_SEC_OFFSET, 0.f);
        alSourcei(Source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSourcei(Source, AL_LOOPING, AL_FALSE);
        if(Renderer->EnvSlot)
        {
            alSourcef(Source, AL_ROOM_ROLLOFF_FACTOR, 0.f);
            alSourcef(Source, AL_AIR_ABSORPTION_FACTOR, 0.f);
            alSourcei(Source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            alSource3i(Source, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
        }

        alGenBuffers(BufferCount, Buffers);
        return (getALError() == AL_NO_ERROR);
    }

public:
    OpenALSoundStream(OpenALSoundRenderer *renderer)
      : Renderer(renderer), Source(0), Playing(false), Looping(false), Volume(1.0f)
    {
        Renderer->Streams.push_back(this);
        memset(Buffers, 0, sizeof(Buffers));
    }

    virtual ~OpenALSoundStream()
    {
        if(Source)
        {
            alSourceRewind(Source);
            alSourcei(Source, AL_BUFFER, 0);

            Renderer->FreeSfx.push_back(Source);
            Source = 0;
        }

        if(Buffers[0])
        {
            alDeleteBuffers(BufferCount, &Buffers[0]);
            memset(Buffers, 0, sizeof(Buffers));
        }
        getALError();

        Renderer->Streams.erase(std::find(Renderer->Streams.begin(),
                                          Renderer->Streams.end(), this));
        Renderer = NULL;
    }


    virtual bool Play(bool loop, float vol)
    {
        SetVolume(vol);

        if(Playing)
            return true;

        /* Clear the buffer queue, then fill and queue each buffer */
        alSourcei(Source, AL_BUFFER, 0);
        for(int i = 0;i < BufferCount;i++)
        {
            if(!Callback(this, &Data[0], Data.size(), UserData))
            {
                if(i == 0)
                    return false;
                break;
            }

            alBufferData(Buffers[i], Format, &Data[0], Data.size(), SampleRate);
            alSourceQueueBuffers(Source, 1, &Buffers[i]);
        }
        if(getALError() != AL_NO_ERROR)
            return false;

        alSourcePlay(Source);
        Playing = (getALError()==AL_NO_ERROR);

        return Playing;
    }

    virtual void Stop()
    {
        if(!Playing)
            return;

        alSourceStop(Source);
        alSourcei(Source, AL_BUFFER, 0);
        getALError();

        Playing = false;
    }

    virtual void SetVolume(float vol)
    {
        if(vol >= 0.0f) Volume = vol;
        alSourcef(Source, AL_GAIN, Renderer->MusicVolume*Volume);
        getALError();
    }

    virtual bool SetPaused(bool pause)
    {
        if(pause)
            alSourcePause(Source);
        else
            alSourcePlay(Source);
        return (getALError()==AL_NO_ERROR);
    }

    virtual bool SetPosition(unsigned int ms_pos)
    {
        if(!Decoder->seek(ms_pos))
            return false;

        if(!Playing)
            return true;
        // Stop the source so that all buffers become processed, then call
        // IsEnded() to refill and restart the source queue with the new
        // position.
        alSourceStop(Source);
        getALError();
        return !IsEnded();
    }

    virtual unsigned int GetPosition()
    {
        ALint offset, queued, state;
        alGetSourcei(Source, AL_SAMPLE_OFFSET, &offset);
        alGetSourcei(Source, AL_BUFFERS_QUEUED, &queued);
        alGetSourcei(Source, AL_SOURCE_STATE, &state);
        if(getALError() != AL_NO_ERROR)
            return 0;

        size_t pos = Decoder->getSampleOffset();
        if(state != AL_STOPPED)
        {
            size_t rem = queued*(Data.size()/FrameSize) - offset;
            if(pos > rem) pos -= rem;
            else pos = 0;
        }
        return (unsigned int)(pos * 1000.0 / SampleRate);
    }

    virtual bool IsEnded()
    {
        if(!Playing)
            return true;

        ALint state, processed;
        alGetSourcei(Source, AL_SOURCE_STATE, &state);
        alGetSourcei(Source, AL_BUFFERS_PROCESSED, &processed);

        Playing = (getALError()==AL_NO_ERROR);
        if(!Playing)
            return true;

        // For each processed buffer in the queue...
        while(processed > 0)
        {
            ALuint bufid;

            // Unqueue the oldest buffer, fill it with more data, and queue it
            // on the end
            alSourceUnqueueBuffers(Source, 1, &bufid);
            processed--;

            if(Callback(this, &Data[0], Data.size(), UserData))
            {
                alBufferData(bufid, Format, &Data[0], Data.size(), SampleRate);
                alSourceQueueBuffers(Source, 1, &bufid);
            }
        }

        // If the source is not playing or paused, and there are buffers queued,
        // then there was an underrun. Restart the source.
        Playing = (getALError()==AL_NO_ERROR);
        if(Playing && state != AL_PLAYING && state != AL_PAUSED)
        {
            ALint queued = 0;
            alGetSourcei(Source, AL_BUFFERS_QUEUED, &queued);

            Playing = (getALError() == AL_NO_ERROR) && (queued > 0);
            if(Playing)
            {
                alSourcePlay(Source);
                Playing = (getALError()==AL_NO_ERROR);
            }
        }

        return !Playing;
    }

    FString GetStats()
    {
        FString stats;
        size_t pos, len;
        ALfloat volume;
        ALint offset;
        ALint processed;
        ALint queued;
        ALint state;
        ALenum err;

        alGetSourcef(Source, AL_GAIN, &volume);
        alGetSourcei(Source, AL_SAMPLE_OFFSET, &offset);
        alGetSourcei(Source, AL_BUFFERS_PROCESSED, &processed);
        alGetSourcei(Source, AL_BUFFERS_QUEUED, &queued);
        alGetSourcei(Source, AL_SOURCE_STATE, &state);
        if((err=alGetError()) != AL_NO_ERROR)
        {
            stats = "Error getting stats: ";
            stats += alGetString(err);
            return stats;
        }

        stats = (state == AL_INITIAL) ? "Buffering" : (state == AL_STOPPED) ? "Underrun" :
                (state == AL_PLAYING || state == AL_PAUSED) ? "Ready" : "Unknown state";

        pos = Decoder->getSampleOffset();
        len = Decoder->getSampleLength();
        if(state == AL_STOPPED)
            offset = BufferCount * (Data.size()/FrameSize);
        else
        {
            size_t rem = queued*(Data.size()/FrameSize) - offset;
            if(pos > rem) pos -= rem;
            else if(len > 0) pos += len - rem;
            else pos = 0;
        }
        pos = (size_t)(pos * 1000.0 / SampleRate);
        len = (size_t)(len * 1000.0 / SampleRate);
        stats.AppendFormat(",%3lu%% buffered", 100 - 100*offset/(BufferCount*(Data.size()/FrameSize)));
        stats.AppendFormat(", %zu.%03zu", pos/1000, pos%1000);
        if(len > 0)
            stats.AppendFormat(" / %zu.%03zu", len/1000, len%1000);
        if(state == AL_PAUSED)
            stats += ", paused";
        if(state == AL_PLAYING)
            stats += ", playing";
        stats.AppendFormat(", %uHz", SampleRate);
        if(!Playing)
            stats += " XX";
        return stats;
    }

    bool Init(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
    {
        if(!SetupSource())
            return false;

        Callback = callback;
        UserData = userdata;
        SampleRate = samplerate;

        Format = AL_NONE;
        if((flags&Bits8)) /* Signed or unsigned? We assume unsigned 8-bit... */
        {
            if((flags&Mono)) Format = AL_FORMAT_MONO8;
            else Format = AL_FORMAT_STEREO8;
        }
        else if((flags&Float))
        {
            if(alIsExtensionPresent("AL_EXT_FLOAT32"))
            {
                if((flags&Mono)) Format = AL_FORMAT_MONO_FLOAT32;
                else Format = AL_FORMAT_STEREO_FLOAT32;
            }
        }
        else if((flags&Bits32))
        {
        }
        else
        {
            if((flags&Mono)) Format = AL_FORMAT_MONO16;
            else Format = AL_FORMAT_STEREO16;
        }

        if(Format == AL_NONE)
        {
            Printf("Unsupported format: 0x%x\n", flags);
            return false;
        }

        FrameSize = 1;
        if((flags&Bits8))
            FrameSize *= 1;
        else if((flags&(Bits32|Float)))
            FrameSize *= 4;
        else
            FrameSize *= 2;

        if((flags&Mono))
            FrameSize *= 1;
        else
            FrameSize *= 2;

        buffbytes += FrameSize-1;
        buffbytes -= buffbytes%FrameSize;
        Data.resize(buffbytes);

        return true;
    }

    bool Init(std::auto_ptr<FileReader> reader, bool loop)
    {
        if(!SetupSource())
            return false;

        Reader = reader;
        Decoder.reset(Renderer->CreateDecoder(Reader.get()));
        if(!Decoder.get()) return false;

        Callback = DecoderCallback;
        UserData = NULL;
        Format = AL_NONE;
        FrameSize = 1;

        ChannelConfig chans;
        SampleType type;
        int srate;

        Decoder->getInfo(&srate, &chans, &type);
        if(chans == ChannelConfig_Mono)
        {
            if(type == SampleType_UInt8) Format = AL_FORMAT_MONO8;
            if(type == SampleType_Int16) Format = AL_FORMAT_MONO16;
            FrameSize *= 1;
        }
        if(chans == ChannelConfig_Stereo)
        {
            if(type == SampleType_UInt8) Format = AL_FORMAT_STEREO8;
            if(type == SampleType_Int16) Format = AL_FORMAT_STEREO16;
            FrameSize *= 2;
        }
        if(type == SampleType_UInt8) FrameSize *= 1;
        if(type == SampleType_Int16) FrameSize *= 2;

        if(Format == AL_NONE)
        {
            Printf("Unsupported audio format (0x%x / 0x%x)\n", chans, type);
            return false;
        }
        SampleRate = srate;
        Looping = loop;

        Data.resize((size_t)(0.2 * SampleRate) * FrameSize);

        return true;
    }
};


extern ReverbContainer *ForcedEnvironment;

#define AREA_SOUND_RADIUS  (128.f)

#define PITCH_MULT (0.7937005f) /* Approx. 4 semitones lower; what Nash suggested */

#define PITCH(pitch) (snd_pitched ? (pitch)/128.f : 1.f)


static float GetRolloff(const FRolloffInfo *rolloff, float distance)
{
    if(distance <= rolloff->MinDistance)
        return 1.f;
    // Logarithmic rolloff has no max distance where it goes silent.
    if(rolloff->RolloffType == ROLLOFF_Log)
        return rolloff->MinDistance /
               (rolloff->MinDistance + rolloff->RolloffFactor*(distance-rolloff->MinDistance));
    if(distance >= rolloff->MaxDistance)
        return 0.f;

    float volume = (rolloff->MaxDistance - distance) / (rolloff->MaxDistance - rolloff->MinDistance);
    if(rolloff->RolloffType == ROLLOFF_Linear)
        return volume;

    if(rolloff->RolloffType == ROLLOFF_Custom && S_SoundCurve != NULL)
        return S_SoundCurve[int(S_SoundCurveSize * (1.f - volume))] / 127.f;
    return (powf(10.f, volume) - 1.f) / 9.f;
}


template<typename T>
static void LoadALFunc(const char *name, T *x)
{ *x = reinterpret_cast<T>(alGetProcAddress(name)); }

#define LOAD_FUNC(x)  (LoadALFunc(#x, &x))
OpenALSoundRenderer::OpenALSoundRenderer()
    : Device(NULL), Context(NULL), SFXPaused(0), PrevEnvironment(NULL), EnvSlot(0)
{
    EnvFilters[0] = EnvFilters[1] = 0;

    Printf("I_InitSound: Initializing OpenAL\n");

    if(strcmp(snd_aldevice, "Default") != 0)
    {
        Device = alcOpenDevice(*snd_aldevice);
        if(!Device)
            Printf(TEXTCOLOR_BLUE" Failed to open device "TEXTCOLOR_BOLD"%s"TEXTCOLOR_BLUE". Trying default.\n", *snd_aldevice);
    }

    if(!Device)
    {
        Device = alcOpenDevice(NULL);
        if(!Device)
        {
            Printf(TEXTCOLOR_RED" Could not open audio device\n");
            return;
        }
    }

    const ALCchar *current = NULL;
    if(alcIsExtensionPresent(Device, "ALC_ENUMERATE_ALL_EXT"))
        current = alcGetString(Device, ALC_ALL_DEVICES_SPECIFIER);
    if(alcGetError(Device) != ALC_NO_ERROR || !current)
        current = alcGetString(Device, ALC_DEVICE_SPECIFIER);
    Printf("  Opened device "TEXTCOLOR_ORANGE"%s\n", current);

    ALCint major=0, minor=0;
    alcGetIntegerv(Device, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(Device, ALC_MINOR_VERSION, 1, &minor);
    DPrintf("  ALC Version: "TEXTCOLOR_BLUE"%d.%d\n", major, minor);
    DPrintf("  ALC Extensions: "TEXTCOLOR_ORANGE"%s\n", alcGetString(Device, ALC_EXTENSIONS));

    std::vector<ALCint> attribs;
    if(*snd_samplerate > 0)
    {
        attribs.push_back(ALC_FREQUENCY);
        attribs.push_back(*snd_samplerate);
    }
    // Make sure one source is capable of stereo output with the rest doing
    // mono, without running out of voices
    attribs.push_back(ALC_MONO_SOURCES);
    attribs.push_back(std::max<ALCint>(*snd_channels, 2) - 1);
    attribs.push_back(ALC_STEREO_SOURCES);
    attribs.push_back(1);
    // Other attribs..?
    attribs.push_back(0);

    Context = alcCreateContext(Device, &attribs[0]);
    if(!Context || alcMakeContextCurrent(Context) == ALC_FALSE)
    {
        Printf(TEXTCOLOR_RED"  Failed to setup context: %s\n", alcGetString(Device, alcGetError(Device)));
        if(Context)
            alcDestroyContext(Context);
        Context = NULL;
        alcCloseDevice(Device);
        Device = NULL;
        return;
    }
    attribs.clear();

    DPrintf("  Vendor: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_VENDOR));
    DPrintf("  Renderer: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_RENDERER));
    DPrintf("  Version: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_VERSION));
    DPrintf("  Extensions: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_EXTENSIONS));

    ALC.EXT_EFX = alcIsExtensionPresent(Device, "ALC_EXT_EFX");
    ALC.EXT_disconnect = alcIsExtensionPresent(Device, "ALC_EXT_disconnect");;
    AL.EXT_source_distance_model = alIsExtensionPresent("AL_EXT_source_distance_model");
    AL.SOFT_deferred_updates = alIsExtensionPresent("AL_SOFT_deferred_updates");
    AL.SOFT_loop_points = alIsExtensionPresent("AL_SOFT_loop_points");

    alDopplerFactor(0.5f);
    alSpeedOfSound(343.3f * 96.0f);
    alDistanceModel(AL_INVERSE_DISTANCE);
    if(AL.EXT_source_distance_model)
        alEnable(AL_SOURCE_DISTANCE_MODEL);

    if(AL.SOFT_deferred_updates)
    {
        LOAD_FUNC(alDeferUpdatesSOFT);
        LOAD_FUNC(alProcessUpdatesSOFT);
    }
    else
    {
        alDeferUpdatesSOFT = _wrap_DeferUpdatesSOFT;
        alProcessUpdatesSOFT = _wrap_ProcessUpdatesSOFT;
    }

    ALenum err = getALError();
    if(err != AL_NO_ERROR)
    {
        alcMakeContextCurrent(NULL);
        alcDestroyContext(Context);
        Context = NULL;
        alcCloseDevice(Device);
        Device = NULL;
        return;
    }

    ALCint numMono=0, numStereo=0;
    alcGetIntegerv(Device, ALC_MONO_SOURCES, 1, &numMono);
    alcGetIntegerv(Device, ALC_STEREO_SOURCES, 1, &numStereo);

    Sources.resize(std::min<size_t>(std::max<ALCint>(*snd_channels, 2),
                                    numMono+numStereo));
    for(size_t i = 0;i < Sources.size();i++)
    {
        alGenSources(1, &Sources[i]);
        if(getALError() != AL_NO_ERROR)
        {
            Sources.resize(i);
            break;
        }
        FreeSfx.push_back(Sources[i]);
    }
    if(Sources.size() == 0)
    {
        Printf(TEXTCOLOR_RED" Error: could not generate any sound sources!\n");
        alcMakeContextCurrent(NULL);
        alcDestroyContext(Context);
        Context = NULL;
        alcCloseDevice(Device);
        Device = NULL;
        return;
    }
    DPrintf("  Allocated "TEXTCOLOR_BLUE"%zu"TEXTCOLOR_NORMAL" sources\n", Sources.size());

    WasInWater = false;
    if(*snd_efx && ALC.EXT_EFX)
    {
        // EFX function pointers
        LOAD_FUNC(alGenEffects);
        LOAD_FUNC(alDeleteEffects);
        LOAD_FUNC(alIsEffect);
        LOAD_FUNC(alEffecti);
        LOAD_FUNC(alEffectiv);
        LOAD_FUNC(alEffectf);
        LOAD_FUNC(alEffectfv);
        LOAD_FUNC(alGetEffecti);
        LOAD_FUNC(alGetEffectiv);
        LOAD_FUNC(alGetEffectf);
        LOAD_FUNC(alGetEffectfv);

        LOAD_FUNC(alGenFilters);
        LOAD_FUNC(alDeleteFilters);
        LOAD_FUNC(alIsFilter);
        LOAD_FUNC(alFilteri);
        LOAD_FUNC(alFilteriv);
        LOAD_FUNC(alFilterf);
        LOAD_FUNC(alFilterfv);
        LOAD_FUNC(alGetFilteri);
        LOAD_FUNC(alGetFilteriv);
        LOAD_FUNC(alGetFilterf);
        LOAD_FUNC(alGetFilterfv);

        LOAD_FUNC(alGenAuxiliaryEffectSlots);
        LOAD_FUNC(alDeleteAuxiliaryEffectSlots);
        LOAD_FUNC(alIsAuxiliaryEffectSlot);
        LOAD_FUNC(alAuxiliaryEffectSloti);
        LOAD_FUNC(alAuxiliaryEffectSlotiv);
        LOAD_FUNC(alAuxiliaryEffectSlotf);
        LOAD_FUNC(alAuxiliaryEffectSlotfv);
        LOAD_FUNC(alGetAuxiliaryEffectSloti);
        LOAD_FUNC(alGetAuxiliaryEffectSlotiv);
        LOAD_FUNC(alGetAuxiliaryEffectSlotf);
        LOAD_FUNC(alGetAuxiliaryEffectSlotfv);
        if(getALError() == AL_NO_ERROR)
        {
            ALuint envReverb;
            alGenEffects(1, &envReverb);
            if(getALError() == AL_NO_ERROR)
            {
                alEffecti(envReverb, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
                if(alGetError() == AL_NO_ERROR)
                    DPrintf("  EAX Reverb found\n");
                alEffecti(envReverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
                if(alGetError() == AL_NO_ERROR)
                    DPrintf("  Standard Reverb found\n");

                alDeleteEffects(1, &envReverb);
                getALError();
            }

            alGenAuxiliaryEffectSlots(1, &EnvSlot);
            alGenFilters(2, EnvFilters);
            if(getALError() == AL_NO_ERROR)
            {
                alFilteri(EnvFilters[0], AL_FILTER_TYPE, AL_FILTER_LOWPASS);
                alFilteri(EnvFilters[1], AL_FILTER_TYPE, AL_FILTER_LOWPASS);
                if(getALError() == AL_NO_ERROR)
                    DPrintf("  Lowpass found\n");
                else
                {
                    alDeleteFilters(2, EnvFilters);
                    EnvFilters[0] = EnvFilters[1] = 0;
                    alDeleteAuxiliaryEffectSlots(1, &EnvSlot);
                    EnvSlot = 0;
                    getALError();
                }
            }
            else
            {
                alDeleteFilters(2, EnvFilters);
                alDeleteAuxiliaryEffectSlots(1, &EnvSlot);
                EnvFilters[0] = EnvFilters[1] = 0;
                EnvSlot = 0;
                getALError();
            }
        }
    }

    if(EnvSlot)
        Printf("  EFX enabled\n");
}
#undef LOAD_FUNC

OpenALSoundRenderer::~OpenALSoundRenderer()
{
    if(!Device)
        return;

    while(Streams.size() > 0)
        delete Streams[0];

    alDeleteSources(Sources.size(), &Sources[0]);
    Sources.clear();
    FreeSfx.clear();
    SfxGroup.clear();
    PausableSfx.clear();
    ReverbSfx.clear();

    for(EffectMap::iterator i = EnvEffects.begin();i != EnvEffects.end();i++)
    {
        if(i->second)
            alDeleteEffects(1, &(i->second));
    }
    EnvEffects.clear();

    if(EnvSlot)
    {
        alDeleteAuxiliaryEffectSlots(1, &EnvSlot);
        alDeleteFilters(2, EnvFilters);
    }
    EnvSlot = 0;
    EnvFilters[0] = EnvFilters[1] = 0;

    alcMakeContextCurrent(NULL);
    alcDestroyContext(Context);
    Context = NULL;
    alcCloseDevice(Device);
    Device = NULL;
}

void OpenALSoundRenderer::SetSfxVolume(float volume)
{
    SfxVolume = volume;

    FSoundChan *schan = Channels;
    while(schan)
    {
        if(schan->SysChannel != NULL)
        {
            ALuint source = GET_PTRID(schan->SysChannel);
            volume = SfxVolume;

            alDeferUpdatesSOFT();
            alSourcef(source, AL_MAX_GAIN, volume);
            alSourcef(source, AL_GAIN, volume * schan->Volume);
        }
        schan = schan->NextChan;
    }

    getALError();
}

void OpenALSoundRenderer::SetMusicVolume(float volume)
{
    MusicVolume = volume;
    foreach(SoundStream*, i, Streams)
        (*i)->SetVolume(-1.f);
}

unsigned int OpenALSoundRenderer::GetMSLength(SoundHandle sfx)
{
    if(sfx.data)
    {
        ALuint buffer = GET_PTRID(sfx.data);
        if(alIsBuffer(buffer))
        {
            ALint bits, channels, freq, size;
            alGetBufferi(buffer, AL_BITS, &bits);
            alGetBufferi(buffer, AL_CHANNELS, &channels);
            alGetBufferi(buffer, AL_FREQUENCY, &freq);
            alGetBufferi(buffer, AL_SIZE, &size);
            if(getALError() == AL_NO_ERROR)
                return (unsigned int)(size / (channels*bits/8) * 1000. / freq);
        }
    }
    return 0;
}

unsigned int OpenALSoundRenderer::GetSampleLength(SoundHandle sfx)
{
    if(sfx.data)
    {
        ALuint buffer = GET_PTRID(sfx.data);
        ALint bits, channels, size;
        alGetBufferi(buffer, AL_BITS, &bits);
        alGetBufferi(buffer, AL_CHANNELS, &channels);
        alGetBufferi(buffer, AL_SIZE, &size);
        if(getALError() == AL_NO_ERROR)
            return (ALsizei)(size / (channels * bits / 8));
    }
    return 0;
}

float OpenALSoundRenderer::GetOutputRate()
{
    ALCint rate = 44100; // Default, just in case
    alcGetIntegerv(Device, ALC_FREQUENCY, 1, &rate);
    return (float)rate;
}


SoundHandle OpenALSoundRenderer::LoadSoundRaw(BYTE *sfxdata, int length, int frequency, int channels, int bits, int loopstart, int loopend)
{
    SoundHandle retval = { NULL };

    if(length == 0) return retval;

    if(bits == -8)
    {
        // Simple signed->unsigned conversion
        for(int i = 0;i < length;i++)
            sfxdata[i] ^= 0x80;
        bits = -bits;
    }

    ALenum format = AL_NONE;
    if(bits == 16)
    {
        if(channels == 1) format = AL_FORMAT_MONO16;
        if(channels == 2) format = AL_FORMAT_STEREO16;
    }
    else if(bits == 8)
    {
        if(channels == 1) format = AL_FORMAT_MONO8;
        if(channels == 2) format = AL_FORMAT_STEREO8;
    }

    if(format == AL_NONE || frequency <= 0)
    {
        Printf("Unhandled format: %d bit, %d channel, %d hz\n", bits, channels, frequency);
        return retval;
    }
    length -= length%(channels*bits/8);

    ALenum err;
    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, sfxdata, length, frequency);
    if((err=getALError()) != AL_NO_ERROR)
    {
        Printf("Failed to buffer data: %s\n", alGetString(err));
        alDeleteBuffers(1, &buffer);
        getALError();
        return retval;
    }

    if((loopstart > 0 || loopend > 0) && AL.SOFT_loop_points)
    {
        if(loopstart < 0)
            loopstart = 0;
        if(loopend < loopstart)
            loopend = length / (channels*bits/8);

        ALint loops[2] = { loopstart, loopend };
        DPrintf("Setting loop points %d -> %d\n", loops[0], loops[1]);
        alBufferiv(buffer, AL_LOOP_POINTS_SOFT, loops);
        getALError();
    }
    else if(loopstart > 0 || loopend > 0)
    {
        static bool warned = false;
        if(!warned)
            Printf("Loop points not supported!\n");
        warned = true;
    }

    retval.data = MAKE_PTRID(buffer);
    return retval;
}

SoundHandle OpenALSoundRenderer::LoadSound(BYTE *sfxdata, int length)
{
    SoundHandle retval = { NULL };
    MemoryReader reader((const char*)sfxdata, length);
    ALenum format = AL_NONE;
    ChannelConfig chans;
    SampleType type;
    int srate;

    std::auto_ptr<SoundDecoder> decoder(CreateDecoder(&reader));
    if(!decoder.get()) return retval;

    decoder->getInfo(&srate, &chans, &type);
    if(chans == ChannelConfig_Mono)
    {
        if(type == SampleType_UInt8) format = AL_FORMAT_MONO8;
        if(type == SampleType_Int16) format = AL_FORMAT_MONO16;
    }
    if(chans == ChannelConfig_Stereo)
    {
        if(type == SampleType_UInt8) format = AL_FORMAT_STEREO8;
        if(type == SampleType_Int16) format = AL_FORMAT_STEREO16;
    }

    if(format == AL_NONE)
    {
        Printf("Unsupported audio format (0x%x / 0x%x)\n", chans, type);
        return retval;
    }

    std::vector<char> data = decoder->readAll();

    ALuint buffer = 0;
    alGenBuffers(1, &buffer);
    alBufferData(buffer, format, &data[0], data.size(), srate);

    ALenum err;
    if((err=getALError()) != AL_NO_ERROR)
    {
        Printf("Failed to buffer data: %s\n", alGetString(err));
        alDeleteBuffers(1, &buffer);
        getALError();
        return retval;
    }

    retval.data = MAKE_PTRID(buffer);
    return retval;
}

void OpenALSoundRenderer::UnloadSound(SoundHandle sfx)
{
    if(!sfx.data)
        return;

    ALuint buffer = GET_PTRID(sfx.data);
    FSoundChan *schan = Channels;
    while(schan)
    {
        if(schan->SysChannel)
        {
            ALint bufID = 0;
            alGetSourcei(GET_PTRID(schan->SysChannel), AL_BUFFER, &bufID);
            if((ALuint)bufID == buffer)
            {
                FSoundChan *next = schan->NextChan;
                StopChannel(schan);
                schan = next;
                continue;
            }
        }
        schan = schan->NextChan;
    }

    alDeleteBuffers(1, &buffer);
    getALError();
}


SoundStream *OpenALSoundRenderer::CreateStream(SoundStreamCallback callback, int buffbytes, int flags, int samplerate, void *userdata)
{
    std::auto_ptr<OpenALSoundStream> stream(new OpenALSoundStream(this));
    if(!stream->Init(callback, buffbytes, flags, samplerate, userdata))
        return NULL;
    return stream.release();
}

SoundStream *OpenALSoundRenderer::OpenStream(std::auto_ptr<FileReader> reader, int flags)
{
    std::auto_ptr<OpenALSoundStream> stream(new OpenALSoundStream(this));

    bool ok = stream->Init(reader, (flags&SoundStream::Loop));
    if(ok == false) return NULL;

    return stream.release();
}

FISoundChannel *OpenALSoundRenderer::StartSound(SoundHandle sfx, float vol, int pitch, int chanflags, FISoundChannel *reuse_chan)
{
    if(FreeSfx.size() == 0)
    {
        FSoundChan *lowest = FindLowestChannel();
        if(lowest) StopChannel(lowest);

        if(FreeSfx.size() == 0)
            return NULL;
    }

    ALuint buffer = GET_PTRID(sfx.data);
    ALuint source = FreeSfx.back();
    alSource3f(source, AL_POSITION, 0.f, 0.f, 0.f);
    alSource3f(source, AL_VELOCITY, 0.f, 0.f, 0.f);
    alSource3f(source, AL_DIRECTION, 0.f, 0.f, 0.f);
    alSourcei(source, AL_SOURCE_RELATIVE, AL_TRUE);

    alSourcei(source, AL_LOOPING, (chanflags&SNDF_LOOP) ? AL_TRUE : AL_FALSE);

    alSourcef(source, AL_REFERENCE_DISTANCE, 1.f);
    alSourcef(source, AL_MAX_DISTANCE, 1000.f);
    alSourcef(source, AL_ROLLOFF_FACTOR, 0.f);
    alSourcef(source, AL_MAX_GAIN, SfxVolume);
    alSourcef(source, AL_GAIN, SfxVolume*vol);

    if(EnvSlot)
    {
        if(!(chanflags&SNDF_NOREVERB))
        {
            alSourcei(source, AL_DIRECT_FILTER, EnvFilters[0]);
            alSource3i(source, AL_AUXILIARY_SEND_FILTER, EnvSlot, 0, EnvFilters[1]);
        }
        else
        {
            alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            alSource3i(source, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
        }
        alSourcef(source, AL_ROOM_ROLLOFF_FACTOR, 0.f);
    }
    if(WasInWater && !(chanflags&SNDF_NOREVERB))
        alSourcef(source, AL_PITCH, PITCH(pitch)*PITCH_MULT);
    else
        alSourcef(source, AL_PITCH, PITCH(pitch));

    if(!reuse_chan)
        alSourcef(source, AL_SEC_OFFSET, 0.f);
    else
    {
        if((chanflags&SNDF_ABSTIME))
            alSourcef(source, AL_SEC_OFFSET, reuse_chan->StartTime.Lo/1000.f);
        else
        {
            // FIXME: set offset based on the current time and the StartTime
            alSourcef(source, AL_SEC_OFFSET, 0.f);
        }
    }
    if(getALError() != AL_NO_ERROR)
        return NULL;

    alSourcei(source, AL_BUFFER, buffer);
    if((chanflags&SNDF_NOPAUSE) || !SFXPaused)
        alSourcePlay(source);
    if(getALError() != AL_NO_ERROR)
    {
        alSourcei(source, AL_BUFFER, 0);
        getALError();
        return NULL;
    }

    if(!(chanflags&SNDF_NOREVERB))
        ReverbSfx.push_back(source);
    if(!(chanflags&SNDF_NOPAUSE))
        PausableSfx.push_back(source);
    SfxGroup.push_back(source);
    FreeSfx.pop_back();

    FISoundChannel *chan = reuse_chan;
    if(!chan) chan = S_GetChannel(MAKE_PTRID(source));
    else chan->SysChannel = MAKE_PTRID(source);

    chan->Rolloff.RolloffType = ROLLOFF_Log;
    chan->Rolloff.RolloffFactor = 0.f;
    chan->Rolloff.MinDistance = 1.f;
    chan->DistanceScale = 1.f;
    chan->DistanceSqr = 0.f;
    chan->ManualRolloff = false;

    return chan;
}

FISoundChannel *OpenALSoundRenderer::StartSound3D(SoundHandle sfx, SoundListener *listener, float vol,
    FRolloffInfo *rolloff, float distscale, int pitch, int priority, const FVector3 &pos, const FVector3 &vel,
    int channum, int chanflags, FISoundChannel *reuse_chan)
{
    float dist_sqr = (pos - listener->position).LengthSquared();

    if(FreeSfx.size() == 0)
    {
        FSoundChan *lowest = FindLowestChannel();
        if(lowest)
        {
            if(lowest->Priority < priority || (lowest->Priority == priority &&
                                               lowest->DistanceSqr > dist_sqr))
                StopChannel(lowest);
        }
        if(FreeSfx.size() == 0)
            return NULL;
    }

    bool manualRolloff = true;
    ALuint buffer = GET_PTRID(sfx.data);
    ALuint source = FreeSfx.back();
    if(rolloff->RolloffType == ROLLOFF_Log)
    {
        if(AL.EXT_source_distance_model)
            alSourcei(source, AL_DISTANCE_MODEL, AL_INVERSE_DISTANCE);
        alSourcef(source, AL_REFERENCE_DISTANCE, rolloff->MinDistance/distscale);
        alSourcef(source, AL_MAX_DISTANCE, (1000.f+rolloff->MinDistance)/distscale);
        alSourcef(source, AL_ROLLOFF_FACTOR, rolloff->RolloffFactor);
        manualRolloff = false;
    }
    else if(rolloff->RolloffType == ROLLOFF_Linear && AL.EXT_source_distance_model)
    {
        alSourcei(source, AL_DISTANCE_MODEL, AL_LINEAR_DISTANCE);
        alSourcef(source, AL_REFERENCE_DISTANCE, rolloff->MinDistance/distscale);
        alSourcef(source, AL_MAX_DISTANCE, rolloff->MaxDistance/distscale);
        alSourcef(source, AL_ROLLOFF_FACTOR, 1.f);
        manualRolloff = false;
    }
    if(manualRolloff)
    {
        // How manual rolloff works:
        //
        // If a sound is using Custom or Doom style rolloff, or Linear style
        // when AL_EXT_source_distance_model is not supported, we have to play
        // around a bit to get appropriate distance attenation. What we do is
        // calculate the attenuation that should be applied, then given an
        // Inverse Distance rolloff model with OpenAL, reverse the calculation
        // to get the distance needed for that much attenuation. The Inverse
        // Distance calculation is:
        //
        // Gain = MinDist / (MinDist + RolloffFactor*(Distance - MinDist))
        //
        // Thus, the reverse is:
        //
        // Distance = (MinDist/Gain - MinDist)/RolloffFactor + MinDist
        //
        // This can be simplified by using a MinDist and RolloffFactor of 1,
        // which makes it:
        //
        // Distance = 1.0f/Gain;
        //
        // The source position is then set that many units away from the
        // listener position, and OpenAL takes care of the rest.
        if(AL.EXT_source_distance_model)
            alSourcei(source, AL_DISTANCE_MODEL, AL_INVERSE_DISTANCE);
        alSourcef(source, AL_REFERENCE_DISTANCE, 1.f);
        alSourcef(source, AL_MAX_DISTANCE, 100000.f);
        alSourcef(source, AL_ROLLOFF_FACTOR, 1.f);

        FVector3 dir = pos - listener->position;
        if(dir.DoesNotApproximatelyEqual(FVector3(0.f, 0.f, 0.f)))
        {
            float gain = GetRolloff(rolloff, sqrt(dist_sqr) * distscale);
            dir.Resize((gain > 0.00001f) ? 1.f/gain : 100000.f);
        }
        if((chanflags&SNDF_AREA) && dist_sqr < AREA_SOUND_RADIUS*AREA_SOUND_RADIUS)
        {
            FVector3 near(0.f, !(dir.Y>=0.f) ? -1.f : 1.f, 0.f);
            float a = sqrt(dist_sqr) / AREA_SOUND_RADIUS;
            dir = near + (dir-near)*a;
        }
        dir += listener->position;

        alSource3f(source, AL_POSITION, dir[0], dir[1], -dir[2]);
    }
    else if((chanflags&SNDF_AREA) && dist_sqr < AREA_SOUND_RADIUS*AREA_SOUND_RADIUS)
    {
        FVector3 dir = pos - listener->position;

        float mindist = rolloff->MinDistance/distscale;
        FVector3 near(0.f, !(dir.Y>=0.f) ? -mindist : mindist, 0.f);
        float a = sqrt(dist_sqr) / AREA_SOUND_RADIUS;
        dir = near + (dir-near)*a;

        dir += listener->position;
        alSource3f(source, AL_POSITION, dir[0], dir[1], -dir[2]);
    }
    else
        alSource3f(source, AL_POSITION, pos[0], pos[1], -pos[2]);
    alSource3f(source, AL_VELOCITY, vel[0], vel[1], -vel[2]);
    alSource3f(source, AL_DIRECTION, 0.f, 0.f, 0.f);

    alSourcei(source, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcei(source, AL_LOOPING, (chanflags&SNDF_LOOP) ? AL_TRUE : AL_FALSE);

    alSourcef(source, AL_MAX_GAIN, SfxVolume);
    alSourcef(source, AL_GAIN, SfxVolume);

    if(EnvSlot)
    {
        if(!(chanflags&SNDF_NOREVERB))
        {
            alSourcei(source, AL_DIRECT_FILTER, EnvFilters[0]);
            alSource3i(source, AL_AUXILIARY_SEND_FILTER, EnvSlot, 0, EnvFilters[1]);
        }
        else
        {
            alSourcei(source, AL_DIRECT_FILTER, AL_FILTER_NULL);
            alSource3i(source, AL_AUXILIARY_SEND_FILTER, 0, 0, AL_FILTER_NULL);
        }
        alSourcef(source, AL_ROOM_ROLLOFF_FACTOR, 0.f);
    }
    if(WasInWater && !(chanflags&SNDF_NOREVERB))
        alSourcef(source, AL_PITCH, PITCH(pitch)*PITCH_MULT);
    else
        alSourcef(source, AL_PITCH, PITCH(pitch));

    if(!reuse_chan)
        alSourcef(source, AL_SEC_OFFSET, 0.f);
    else
    {
        if((chanflags&SNDF_ABSTIME))
            alSourcef(source, AL_SEC_OFFSET, reuse_chan->StartTime.Lo/1000.f);
        else
        {
            // FIXME: set offset based on the current time and the StartTime
            alSourcef(source, AL_SAMPLE_OFFSET, 0.f);
        }
    }
    if(getALError() != AL_NO_ERROR)
        return NULL;

    alSourcei(source, AL_BUFFER, buffer);
    if((chanflags&SNDF_NOPAUSE) || !SFXPaused)
        alSourcePlay(source);
    if(getALError() != AL_NO_ERROR)
    {
        alSourcei(source, AL_BUFFER, 0);
        getALError();
        return NULL;
    }

    if(!(chanflags&SNDF_NOREVERB))
        ReverbSfx.push_back(source);
    if(!(chanflags&SNDF_NOPAUSE))
        PausableSfx.push_back(source);
    SfxGroup.push_back(source);
    FreeSfx.pop_back();

    FISoundChannel *chan = reuse_chan;
    if(!chan) chan = S_GetChannel(MAKE_PTRID(source));
    else chan->SysChannel = MAKE_PTRID(source);

    chan->Rolloff = *rolloff;
    chan->DistanceScale = distscale;
    chan->DistanceSqr = dist_sqr;
    chan->ManualRolloff = manualRolloff;

    return chan;
}

void OpenALSoundRenderer::ChannelVolume(FISoundChannel *chan, float volume)
{
    if(chan == NULL || chan->SysChannel == NULL)
        return;

    alDeferUpdatesSOFT();

    ALuint source = GET_PTRID(chan->SysChannel);
    alSourcef(source, AL_GAIN, SfxVolume * volume);
}

void OpenALSoundRenderer::StopChannel(FISoundChannel *chan)
{
    if(chan == NULL || chan->SysChannel == NULL)
        return;

    ALuint source = GET_PTRID(chan->SysChannel);
    // Release first, so it can be properly marked as evicted if it's being
    // forcefully killed
    S_ChannelEnded(chan);

    alSourceRewind(source);
    alSourcei(source, AL_BUFFER, 0);
    getALError();

    std::vector<ALuint>::iterator i;

    i = std::find(PausableSfx.begin(), PausableSfx.end(), source);
    if(i != PausableSfx.end()) PausableSfx.erase(i);
    i = std::find(ReverbSfx.begin(), ReverbSfx.end(), source);
    if(i != ReverbSfx.end()) ReverbSfx.erase(i);

    SfxGroup.erase(std::find(SfxGroup.begin(), SfxGroup.end(), source));
    FreeSfx.push_back(source);
}

unsigned int OpenALSoundRenderer::GetPosition(FISoundChannel *chan)
{
    if(chan == NULL || chan->SysChannel == NULL)
        return 0;

    ALint pos;
    alGetSourcei(GET_PTRID(chan->SysChannel), AL_SAMPLE_OFFSET, &pos);
    if(getALError() == AL_NO_ERROR)
        return pos;
    return 0;
}


void OpenALSoundRenderer::SetSfxPaused(bool paused, int slot)
{
    int oldslots = SFXPaused;

    if(paused)
    {
        SFXPaused |= 1 << slot;
        if(oldslots == 0 && PausableSfx.size() > 0)
        {
            alSourcePausev(PausableSfx.size(), &PausableSfx[0]);
            getALError();
            PurgeStoppedSources();
        }
    }
    else
    {
        SFXPaused &= ~(1 << slot);
        if(SFXPaused == 0 && oldslots != 0 && PausableSfx.size() > 0)
        {
            alSourcePlayv(PausableSfx.size(), &PausableSfx[0]);
            getALError();
        }
    }
}

void OpenALSoundRenderer::SetInactive(SoundRenderer::EInactiveState state)
{
    switch(state)
    {
        case SoundRenderer::INACTIVE_Active:
            alListenerf(AL_GAIN, 1.0f);
            break;

        /* FIXME: This doesn't stop anything. */
        case SoundRenderer::INACTIVE_Complete:
        case SoundRenderer::INACTIVE_Mute:
            alListenerf(AL_GAIN, 0.0f);
            break;
    }
}

void OpenALSoundRenderer::Sync(bool sync)
{
    if(sync)
    {
        if(SfxGroup.size() > 0)
        {
            alSourcePausev(SfxGroup.size(), &SfxGroup[0]);
            getALError();
            PurgeStoppedSources();
        }
    }
    else
    {
        // Might already be something to handle this; basically, get a vector
        // of all values in SfxGroup that are not also in PausableSfx (when
        // SFXPaused is non-0).
        std::vector<ALuint> toplay = SfxGroup;
        if(SFXPaused)
        {
            std::vector<ALuint>::iterator i = toplay.begin();
            while(i != toplay.end())
            {
                if(std::find(PausableSfx.begin(), PausableSfx.end(), *i) != PausableSfx.end())
                    i = toplay.erase(i);
                else
                    i++;
            }
        }
        if(toplay.size() > 0)
        {
            alSourcePlayv(toplay.size(), &toplay[0]);
            getALError();
        }
    }
}

void OpenALSoundRenderer::UpdateSoundParams3D(SoundListener *listener, FISoundChannel *chan, bool areasound, const FVector3 &pos, const FVector3 &vel)
{
    if(chan == NULL || chan->SysChannel == NULL)
        return;

    alDeferUpdatesSOFT();

    FVector3 dir = pos - listener->position;
    chan->DistanceSqr = dir.LengthSquared();

    if(chan->ManualRolloff)
    {
        if(dir.DoesNotApproximatelyEqual(FVector3(0.f, 0.f, 0.f)))
        {
            float gain = GetRolloff(&chan->Rolloff, sqrt(chan->DistanceSqr) * chan->DistanceScale);
            dir.Resize((gain > 0.00001f) ? 1.f/gain : 100000.f);
        }
        if(areasound && chan->DistanceSqr < AREA_SOUND_RADIUS*AREA_SOUND_RADIUS)
        {
            FVector3 near(0.f, !(dir.Y>=0.f) ? -1.f : 1.f, 0.f);
            float a = sqrt(chan->DistanceSqr) / AREA_SOUND_RADIUS;
            dir = near + (dir-near)*a;
        }
    }
    else if(areasound && chan->DistanceSqr < AREA_SOUND_RADIUS*AREA_SOUND_RADIUS)
    {
        float mindist = chan->Rolloff.MinDistance / chan->DistanceScale;
        FVector3 near(0.f, !(dir.Y>=0.f) ? -mindist : mindist, 0.f);
        float a = sqrt(chan->DistanceSqr) / AREA_SOUND_RADIUS;
        dir = near + (dir-near)*a;
    }
    dir += listener->position;

    ALuint source = GET_PTRID(chan->SysChannel);
    alSource3f(source, AL_POSITION, dir[0], dir[1], -dir[2]);
    alSource3f(source, AL_VELOCITY, vel[0], vel[1], -vel[2]);
    getALError();
}

void OpenALSoundRenderer::UpdateListener(SoundListener *listener)
{
    if(!listener->valid)
        return;

    alDeferUpdatesSOFT();

    float angle = listener->angle;
    ALfloat orient[6];
    // forward
    orient[0] = cos(angle);
    orient[1] = 0.f;
    orient[2] = -sin(angle);
    // up
    orient[3] = 0.f;
    orient[4] = 1.f;
    orient[5] = 0.f;

    alListenerfv(AL_ORIENTATION, orient);
    alListener3f(AL_POSITION, listener->position.X,
                              listener->position.Y,
                             -listener->position.Z);
    alListener3f(AL_VELOCITY, listener->velocity.X,
                              listener->velocity.Y,
                             -listener->velocity.Z);
    getALError();

    const ReverbContainer *env = ForcedEnvironment;
    if(!env)
    {
        env = listener->Environment;
        if(!env)
            env = DefaultEnvironments[0];
    }
    if(env != PrevEnvironment || env->Modified)
    {
        PrevEnvironment = env;
        DPrintf("Reverb Environment %s\n", env->Name);

        if(EnvSlot != 0)
            LoadReverb(env);

        const_cast<ReverbContainer*>(env)->Modified = false;
    }

    // NOTE: Moving into and out of water will undo pitch variations on sounds.
    if(listener->underwater || env->SoftwareWater)
    {
        if(!WasInWater)
        {
            WasInWater = true;

            if(EnvSlot != 0 && *snd_waterreverb)
            {
                // Find the "Underwater" reverb environment
                env = S_FindEnvironment(0x1600);
                LoadReverb(env ? env : DefaultEnvironments[0]);

                alFilterf(EnvFilters[0], AL_LOWPASS_GAIN, 1.f);
                alFilterf(EnvFilters[0], AL_LOWPASS_GAINHF, 0.125f);
                alFilterf(EnvFilters[1], AL_LOWPASS_GAIN, 1.f);
                alFilterf(EnvFilters[1], AL_LOWPASS_GAINHF, 1.f);

                // Apply the updated filters on the sources
                foreach(ALuint, i, ReverbSfx)
                {
                    alSourcei(*i, AL_DIRECT_FILTER, EnvFilters[0]);
                    alSource3i(*i, AL_AUXILIARY_SEND_FILTER, EnvSlot, 0, EnvFilters[1]);
                }
            }

            foreach(ALuint, i, ReverbSfx)
                alSourcef(*i, AL_PITCH, PITCH_MULT);
            getALError();
        }
    }
    else if(WasInWater)
    {
        WasInWater = false;

        if(EnvSlot != 0)
        {
            LoadReverb(env);

            alFilterf(EnvFilters[0], AL_LOWPASS_GAIN, 1.f);
            alFilterf(EnvFilters[0], AL_LOWPASS_GAINHF, 1.f);
            alFilterf(EnvFilters[1], AL_LOWPASS_GAIN, 1.f);
            alFilterf(EnvFilters[1], AL_LOWPASS_GAINHF, 1.f);
            foreach(ALuint, i, ReverbSfx)
            {
                alSourcei(*i, AL_DIRECT_FILTER, EnvFilters[0]);
                alSource3i(*i, AL_AUXILIARY_SEND_FILTER, EnvSlot, 0, EnvFilters[1]);
            }
        }

        foreach(ALuint, i, ReverbSfx)
            alSourcef(*i, AL_PITCH, 1.f);
        getALError();
    }
}

void OpenALSoundRenderer::UpdateSounds()
{
    alProcessUpdatesSOFT();

    // For some reason this isn't being called?
    foreach(SoundStream*, stream, Streams)
        (*stream)->IsEnded();

    if(ALC.EXT_disconnect)
    {
        ALCint connected = ALC_TRUE;
        alcGetIntegerv(Device, ALC_CONNECTED, 1, &connected);
        if(connected == ALC_FALSE)
        {
            Printf("Sound device disconnected; restarting...\n");
            static char snd_reset[] = "snd_reset";
            AddCommandString(snd_reset);
            return;
        }
    }

    PurgeStoppedSources();
}

bool OpenALSoundRenderer::IsValid()
{
    return Device != NULL;
}

void OpenALSoundRenderer::MarkStartTime(FISoundChannel *chan)
{
    // FIXME: Get current time (preferably from the audio clock, but the system
    // time will have to do)
    chan->StartTime.AsOne = 0;
}

float OpenALSoundRenderer::GetAudibility(FISoundChannel *chan)
{
    if(chan == NULL || chan->SysChannel == NULL)
        return 0.f;

    ALuint source = GET_PTRID(chan->SysChannel);
    ALfloat volume = 0.f;

    alGetSourcef(source, AL_GAIN, &volume);
    getALError();

    volume *= GetRolloff(&chan->Rolloff, sqrt(chan->DistanceSqr) * chan->DistanceScale);
    return volume;
}


void OpenALSoundRenderer::PrintStatus()
{
    Printf("Output device: "TEXTCOLOR_ORANGE"%s\n", alcGetString(Device, ALC_DEVICE_SPECIFIER));
    getALCError(Device);

    ALCint frequency, major, minor, mono, stereo;
    alcGetIntegerv(Device, ALC_FREQUENCY, 1, &frequency);
    alcGetIntegerv(Device, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(Device, ALC_MINOR_VERSION, 1, &minor);
    alcGetIntegerv(Device, ALC_MONO_SOURCES, 1, &mono);
    alcGetIntegerv(Device, ALC_STEREO_SOURCES, 1, &stereo);
    if(getALCError(Device) == AL_NO_ERROR)
    {
        Printf("Device sample rate: "TEXTCOLOR_BLUE"%d"TEXTCOLOR_NORMAL"hz\n", frequency);
        Printf("ALC Version: "TEXTCOLOR_BLUE"%d.%d\n", major, minor);
        Printf("ALC Extensions: "TEXTCOLOR_ORANGE"%s\n", alcGetString(Device, ALC_EXTENSIONS));
        Printf("Available sources: "TEXTCOLOR_BLUE"%d"TEXTCOLOR_NORMAL" ("TEXTCOLOR_BLUE"%d"TEXTCOLOR_NORMAL" mono, "TEXTCOLOR_BLUE"%d"TEXTCOLOR_NORMAL" stereo)\n", mono+stereo, mono, stereo);
    }
    if(!alcIsExtensionPresent(Device, "ALC_EXT_EFX"))
        Printf("EFX not found\n");
    else
    {
        ALCint sends;
        alcGetIntegerv(Device, ALC_EFX_MAJOR_VERSION, 1, &major);
        alcGetIntegerv(Device, ALC_EFX_MINOR_VERSION, 1, &minor);
        alcGetIntegerv(Device, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
        if(getALCError(Device) == AL_NO_ERROR)
        {
            Printf("EFX Version: "TEXTCOLOR_BLUE"%d.%d\n", major, minor);
            Printf("Auxiliary sends: "TEXTCOLOR_BLUE"%d\n", sends);
        }
    }
    Printf("Vendor: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_VENDOR));
    Printf("Renderer: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_RENDERER));
    Printf("Version: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_VERSION));
    Printf("Extensions: "TEXTCOLOR_ORANGE"%s\n", alGetString(AL_EXTENSIONS));
    getALError();
}

FString OpenALSoundRenderer::GatherStats()
{
    ALCint updates = 1;
    alcGetIntegerv(Device, ALC_REFRESH, 1, &updates);
    getALCError(Device);

    ALuint total = Sources.size();
    ALuint used = SfxGroup.size()+Streams.size();
    ALuint unused = FreeSfx.size();
    FString out;
    out.Format("%u sources ("TEXTCOLOR_YELLOW"%u"TEXTCOLOR_NORMAL" active, "TEXTCOLOR_YELLOW"%u"TEXTCOLOR_NORMAL" free), Update interval: "TEXTCOLOR_YELLOW"%d"TEXTCOLOR_NORMAL"ms",
               total, used, unused, 1000/updates);
    return out;
}

void OpenALSoundRenderer::PrintDriversList()
{
    const ALCchar *drivers = (alcIsExtensionPresent(NULL, "ALC_ENUMERATE_ALL_EXT") ?
                              alcGetString(NULL, ALC_ALL_DEVICES_SPECIFIER) :
                              alcGetString(NULL, ALC_DEVICE_SPECIFIER));
    if(drivers == NULL)
    {
        Printf(TEXTCOLOR_YELLOW"Failed to retrieve device list: %s\n", alcGetString(NULL, alcGetError(NULL)));
        return;
    }

    const ALCchar *current = NULL;
    if(alcIsExtensionPresent(Device, "ALC_ENUMERATE_ALL_EXT"))
        current = alcGetString(Device, ALC_ALL_DEVICES_SPECIFIER);
    if(alcGetError(Device) != ALC_NO_ERROR || !current)
        current = alcGetString(Device, ALC_DEVICE_SPECIFIER);
    if(current == NULL)
    {
        Printf(TEXTCOLOR_YELLOW"Failed to retrieve device name: %s\n", alcGetString(Device, alcGetError(Device)));
        return;
    }

    Printf("%c%s%2d. %s\n", ' ', ((strcmp(snd_aldevice, "Default") == 0) ? TEXTCOLOR_BOLD : ""), 0,
           "Default");
    for(int i = 1;*drivers;i++)
    {
        Printf("%c%s%2d. %s\n", ((strcmp(current, drivers)==0) ? '*' : ' '),
               ((strcmp(*snd_aldevice, drivers)==0) ? TEXTCOLOR_BOLD : ""), i,
               drivers);
        drivers += strlen(drivers)+1;
    }
}

void OpenALSoundRenderer::PurgeStoppedSources()
{
    // Release channels that are stopped
    foreach(ALuint, i, SfxGroup)
    {
        ALint state = AL_INITIAL;
        alGetSourcei(*i, AL_SOURCE_STATE, &state);
        if(state == AL_INITIAL || state == AL_PLAYING || state == AL_PAUSED)
            continue;

        FSoundChan *schan = Channels;
        while(schan)
        {
            if(schan->SysChannel != NULL && *i == GET_PTRID(schan->SysChannel))
            {
                StopChannel(schan);
                break;
            }
            schan = schan->NextChan;
        }
    }
    getALError();
}

void OpenALSoundRenderer::LoadReverb(const ReverbContainer *env)
{
    ALuint &envReverb = EnvEffects[env->ID];
    bool doLoad = (env->Modified || !envReverb);

    if(!envReverb)
    {
        bool ok = false;
        alGenEffects(1, &envReverb);
        if(getALError() == AL_NO_ERROR)
        {
            alEffecti(envReverb, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
            ok = (alGetError() == AL_NO_ERROR);
            if(!ok)
            {
                alEffecti(envReverb, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
                ok = (alGetError() == AL_NO_ERROR);
            }
            if(!ok)
            {
                alEffecti(envReverb, AL_EFFECT_TYPE, AL_EFFECT_NULL);
                ok = (alGetError() == AL_NO_ERROR);
            }
            if(!ok)
            {
                alDeleteEffects(1, &envReverb);
                getALError();
            }
        }
        if(!ok)
        {
            envReverb = 0;
            doLoad = false;
        }
    }

    if(doLoad)
    {
        const REVERB_PROPERTIES &props = env->Properties;
        ALint type = AL_EFFECT_NULL;

        alGetEffecti(envReverb, AL_EFFECT_TYPE, &type);
#define mB2Gain(x) ((float)pow(10., (x)/2000.))
        if(type == AL_EFFECT_EAXREVERB)
        {
            ALfloat reflectpan[3] = { props.ReflectionsPan0,
                                      props.ReflectionsPan1,
                                      props.ReflectionsPan2 };
            ALfloat latepan[3] = { props.ReverbPan0, props.ReverbPan1,
                                   props.ReverbPan2 };
#undef SETPARAM
#define SETPARAM(e,t,v) alEffectf((e), AL_EAXREVERB_##t, clamp((v), AL_EAXREVERB_MIN_##t, AL_EAXREVERB_MAX_##t))
            SETPARAM(envReverb, DIFFUSION, props.EnvDiffusion);
            SETPARAM(envReverb, GAIN, mB2Gain(props.Room));
            SETPARAM(envReverb, GAINHF, mB2Gain(props.RoomHF));
            SETPARAM(envReverb, GAINLF, mB2Gain(props.RoomLF));
            SETPARAM(envReverb, DECAY_TIME, props.DecayTime);
            SETPARAM(envReverb, DECAY_HFRATIO, props.DecayHFRatio);
            SETPARAM(envReverb, DECAY_LFRATIO, props.DecayLFRatio);
            SETPARAM(envReverb, REFLECTIONS_GAIN, mB2Gain(props.Reflections));
            SETPARAM(envReverb, REFLECTIONS_DELAY, props.ReflectionsDelay);
            alEffectfv(envReverb, AL_EAXREVERB_REFLECTIONS_PAN, reflectpan);
            SETPARAM(envReverb, LATE_REVERB_GAIN, mB2Gain(props.Reverb));
            SETPARAM(envReverb, LATE_REVERB_DELAY, props.ReverbDelay);
            alEffectfv(envReverb, AL_EAXREVERB_LATE_REVERB_PAN, latepan);
            SETPARAM(envReverb, ECHO_TIME, props.EchoTime);
            SETPARAM(envReverb, ECHO_DEPTH, props.EchoDepth);
            SETPARAM(envReverb, MODULATION_TIME, props.ModulationTime);
            SETPARAM(envReverb, MODULATION_DEPTH, props.ModulationDepth);
            SETPARAM(envReverb, AIR_ABSORPTION_GAINHF, mB2Gain(props.AirAbsorptionHF));
            SETPARAM(envReverb, HFREFERENCE, props.HFReference);
            SETPARAM(envReverb, LFREFERENCE, props.LFReference);
            SETPARAM(envReverb, ROOM_ROLLOFF_FACTOR, props.RoomRolloffFactor);
            alEffecti(envReverb, AL_EAXREVERB_DECAY_HFLIMIT,
                                 (props.Flags&REVERB_FLAGS_DECAYHFLIMIT)?AL_TRUE:AL_FALSE);
#undef SETPARAM
        }
        else if(type == AL_EFFECT_REVERB)
        {
#define SETPARAM(e,t,v) alEffectf((e), AL_REVERB_##t, clamp((v), AL_REVERB_MIN_##t, AL_REVERB_MAX_##t))
            SETPARAM(envReverb, DIFFUSION, props.EnvDiffusion);
            SETPARAM(envReverb, GAIN, mB2Gain(props.Room));
            SETPARAM(envReverb, GAINHF, mB2Gain(props.RoomHF));
            SETPARAM(envReverb, DECAY_TIME, props.DecayTime);
            SETPARAM(envReverb, DECAY_HFRATIO, props.DecayHFRatio);
            SETPARAM(envReverb, REFLECTIONS_GAIN, mB2Gain(props.Reflections));
            SETPARAM(envReverb, REFLECTIONS_DELAY, props.ReflectionsDelay);
            SETPARAM(envReverb, LATE_REVERB_GAIN, mB2Gain(props.Reverb));
            SETPARAM(envReverb, LATE_REVERB_DELAY, props.ReverbDelay);
            SETPARAM(envReverb, AIR_ABSORPTION_GAINHF, mB2Gain(props.AirAbsorptionHF));
            SETPARAM(envReverb, ROOM_ROLLOFF_FACTOR, props.RoomRolloffFactor);
            alEffecti(envReverb, AL_REVERB_DECAY_HFLIMIT,
                                 (props.Flags&REVERB_FLAGS_DECAYHFLIMIT)?AL_TRUE:AL_FALSE);
#undef SETPARAM
        }
#undef mB2Gain
    }

    alAuxiliaryEffectSloti(EnvSlot, AL_EFFECTSLOT_EFFECT, envReverb);
    getALError();
}

FSoundChan *OpenALSoundRenderer::FindLowestChannel()
{
    FSoundChan *schan = Channels;
    FSoundChan *lowest = NULL;
    while(schan)
    {
        if(schan->SysChannel != NULL)
        {
            if(!lowest || schan->Priority < lowest->Priority ||
               (schan->Priority == lowest->Priority &&
                schan->DistanceSqr > lowest->DistanceSqr))
                lowest = schan;
        }
        schan = schan->NextChan;
    }
    return lowest;
}

#endif // NO_OPENAL