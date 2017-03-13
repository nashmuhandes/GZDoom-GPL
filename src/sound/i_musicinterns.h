#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define USE_WINDOWS_DWORD
#if defined(_WIN32_WINNT) && _WIN32_WINNT < 0x0400
#undef _WIN32_WINNT
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0400
#endif
#ifndef USE_WINDOWS_DWORD
#define USE_WINDOWS_DWORD
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#define FALSE 0
#define TRUE 1
#endif
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#endif // __APPLE__
#include "tempfiles.h"
#include "critsec.h"
#include "c_cvars.h"
#include "mus2midi.h"
#include "i_sound.h"
#include "i_music.h"
#include "s_sound.h"
#include "files.h"
#include "wildmidi/wildmidi_lib.h"

void I_InitMusicWin32 ();
void I_ShutdownMusicWin32 ();

extern float relative_volume;

EXTERN_CVAR (Float, timidity_mastervolume)


// A device that provides a WinMM-like MIDI streaming interface -------------

#ifndef _WIN32
struct MIDIHDR
{
	BYTE *lpData;
	DWORD dwBufferLength;
	DWORD dwBytesRecorded;
	MIDIHDR *lpNext;
};

enum
{
	MOD_MIDIPORT = 1,
	MOD_SYNTH,
	MOD_SQSYNTH,
	MOD_FMSYNTH,
	MOD_MAPPER,
	MOD_WAVETABLE,
	MOD_SWSYNTH
};

typedef BYTE *LPSTR;

#define MEVT_TEMPO			((BYTE)1)
#define MEVT_NOP			((BYTE)2)
#define MEVT_LONGMSG		((BYTE)128)

#define MEVT_EVENTTYPE(x)	((BYTE)((x) >> 24))
#define MEVT_EVENTPARM(x)   ((x) & 0xffffff)

#define MOM_DONE			969
#else
// w32api does not define these
#ifndef MOD_WAVETABLE
#define MOD_WAVETABLE   6
#define MOD_SWSYNTH     7
#endif
#endif

class MIDIStreamer;

class MIDIDevice
{
public:
	MIDIDevice();
	virtual ~MIDIDevice();

	virtual int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata) = 0;
	virtual void Close() = 0;
	virtual bool IsOpen() const = 0;
	virtual int GetTechnology() const = 0;
	virtual int SetTempo(int tempo) = 0;
	virtual int SetTimeDiv(int timediv) = 0;
	virtual int StreamOut(MIDIHDR *data) = 0;
	virtual int StreamOutSync(MIDIHDR *data) = 0;
	virtual int Resume() = 0;
	virtual void Stop() = 0;
	virtual int PrepareHeader(MIDIHDR *data);
	virtual int UnprepareHeader(MIDIHDR *data);
	virtual bool FakeVolume();
	virtual bool Pause(bool paused) = 0;
	virtual bool NeedThreadedCallback();
	virtual void PrecacheInstruments(const uint16_t *instruments, int count);
	virtual void TimidityVolumeChanged();
	virtual void FluidSettingInt(const char *setting, int value);
	virtual void FluidSettingNum(const char *setting, double value);
	virtual void FluidSettingStr(const char *setting, const char *value);
	virtual void WildMidiSetOption(int opt, int set);
	virtual bool Preprocess(MIDIStreamer *song, bool looping);
	virtual FString GetStats();
};

// WinMM implementation of a MIDI output device -----------------------------

#ifdef _WIN32
class WinMIDIDevice : public MIDIDevice
{
public:
	WinMIDIDevice(int dev_id);
	~WinMIDIDevice();
	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	void Close();
	bool IsOpen() const;
	int GetTechnology() const;
	int SetTempo(int tempo);
	int SetTimeDiv(int timediv);
	int StreamOut(MIDIHDR *data);
	int StreamOutSync(MIDIHDR *data);
	int Resume();
	void Stop();
	int PrepareHeader(MIDIHDR *data);
	int UnprepareHeader(MIDIHDR *data);
	bool FakeVolume();
	bool NeedThreadedCallback();
	bool Pause(bool paused);
	void PrecacheInstruments(const uint16_t *instruments, int count);

protected:
	static void CALLBACK CallbackFunc(HMIDIOUT, UINT, DWORD_PTR, DWORD, DWORD);

	HMIDISTRM MidiOut;
	UINT DeviceID;
	DWORD SavedVolume;
	bool VolumeWorks;

	void (*Callback)(unsigned int, void *, DWORD, DWORD);
	void *CallbackData;
};
#endif

// AudioToolbox implementation of a MIDI output device ----------------------

#ifdef __APPLE__

class AudioToolboxMIDIDevice : public MIDIDevice
{
public:
	virtual int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userData) override;
	virtual void Close() override;
	virtual bool IsOpen() const override;
	virtual int GetTechnology() const override;
	virtual int SetTempo(int tempo) override;
	virtual int SetTimeDiv(int timediv) override;
	virtual int StreamOut(MIDIHDR *data) override;
	virtual int StreamOutSync(MIDIHDR *data) override;
	virtual int Resume() override;
	virtual void Stop() override;
	virtual int PrepareHeader(MIDIHDR* data) override;
	virtual bool FakeVolume() override { return true; }
	virtual bool Pause(bool paused) override;
	virtual bool Preprocess(MIDIStreamer *song, bool looping) override;

private:
	MusicPlayer m_player = nullptr;
	MusicSequence m_sequence = nullptr;
	AudioUnit m_audioUnit = nullptr;
	CFRunLoopTimerRef m_timer = nullptr;
	MusicTimeStamp m_length = 0;

	typedef void (*Callback)(unsigned int, void *, DWORD, DWORD);
	Callback m_callback = nullptr;
	void* m_userData = nullptr;

	static void TimerCallback(CFRunLoopTimerRef timer, void* info);
};

#endif // __APPLE__

// Base class for pseudo-MIDI devices ---------------------------------------

class PseudoMIDIDevice : public MIDIDevice
{
public:
	PseudoMIDIDevice();
	~PseudoMIDIDevice();

	void Close();
	bool IsOpen() const;
	int GetTechnology() const;
	bool Pause(bool paused);
	int Resume();
	void Stop();
	int StreamOut(MIDIHDR *data);
	int StreamOutSync(MIDIHDR *data);
	int SetTempo(int tempo);
	int SetTimeDiv(int timediv);
	FString GetStats();

protected:
	SoundStream *Stream;
	bool Started;
	bool bLooping;
};

// Sound System pseudo-MIDI device ------------------------------------------

class SndSysMIDIDevice : public PseudoMIDIDevice
{
public:
	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	bool Preprocess(MIDIStreamer *song, bool looping);
};

// MIDI file played with TiMidity++ and possibly streamed through the Sound System

class TimidityPPMIDIDevice : public PseudoMIDIDevice
{
public:
	TimidityPPMIDIDevice(const char *args);
	~TimidityPPMIDIDevice();

	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	bool Preprocess(MIDIStreamer *song, bool looping);
	bool IsOpen() const;
	int Resume();

	void Stop();
	bool IsOpen();
	void TimidityVolumeChanged();

protected:
	bool LaunchTimidity();

	FTempFileName DiskName;
#ifdef _WIN32
	HANDLE ReadWavePipe;
	HANDLE WriteWavePipe;
	HANDLE ChildProcess;
	bool Validated;
	bool ValidateTimidity();
#else // _WIN32
	int WavePipe[2];
	pid_t ChildProcess;
#endif
	FString CommandLine;
	size_t LoopPos;

	static bool FillStream(SoundStream *stream, void *buff, int len, void *userdata);
#ifdef _WIN32
	static const char EventName[];
#endif
};

// Base class for software synthesizer MIDI output devices ------------------

class SoftSynthMIDIDevice : public MIDIDevice
{
public:
	SoftSynthMIDIDevice();
	~SoftSynthMIDIDevice();

	void Close();
	bool IsOpen() const;
	int GetTechnology() const;
	int SetTempo(int tempo);
	int SetTimeDiv(int timediv);
	int StreamOut(MIDIHDR *data);
	int StreamOutSync(MIDIHDR *data);
	int Resume();
	void Stop();
	bool Pause(bool paused);

protected:
	FCriticalSection CritSec;
	SoundStream *Stream;
	double Tempo;
	double Division;
	double SamplesPerTick;
	double NextTickIn;
	MIDIHDR *Events;
	bool Started;
	DWORD Position;
	int SampleRate;

	void (*Callback)(unsigned int, void *, DWORD, DWORD);
	void *CallbackData;

	virtual void CalcTickRate();
	int PlayTick();
	int OpenStream(int chunks, int flags, void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	static bool FillStream(SoundStream *stream, void *buff, int len, void *userdata);
	virtual bool ServiceStream (void *buff, int numbytes);

	virtual void HandleEvent(int status, int parm1, int parm2) = 0;
	virtual void HandleLongEvent(const BYTE *data, int len) = 0;
	virtual void ComputeOutput(float *buffer, int len) = 0;
};

// Internal TiMidity MIDI device --------------------------------------------

namespace Timidity { struct Renderer; }

class TimidityMIDIDevice : public SoftSynthMIDIDevice
{
public:
	TimidityMIDIDevice(const char *args);
	~TimidityMIDIDevice();

	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	void PrecacheInstruments(const uint16_t *instruments, int count);
	FString GetStats();

protected:
	Timidity::Renderer *Renderer;

	void HandleEvent(int status, int parm1, int parm2);
	void HandleLongEvent(const BYTE *data, int len);
	void ComputeOutput(float *buffer, int len);
};

// Internal TiMidity disk writing version of a MIDI device ------------------

class TimidityWaveWriterMIDIDevice : public TimidityMIDIDevice
{
public:
	TimidityWaveWriterMIDIDevice(const char *filename, int rate);
	~TimidityWaveWriterMIDIDevice();
	int Resume();
	void Stop();

protected:
	FILE *File;
};

// WildMidi implementation of a MIDI device ---------------------------------

class WildMIDIDevice : public SoftSynthMIDIDevice
{
public:
	WildMIDIDevice(const char *args);
	~WildMIDIDevice();

	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	void PrecacheInstruments(const uint16_t *instruments, int count);
	FString GetStats();

protected:
	WildMidi_Renderer *Renderer;

	void HandleEvent(int status, int parm1, int parm2);
	void HandleLongEvent(const BYTE *data, int len);
	void ComputeOutput(float *buffer, int len);
	void WildMidiSetOption(int opt, int set);
};

// FluidSynth implementation of a MIDI device -------------------------------

#ifdef HAVE_FLUIDSYNTH
#ifndef DYN_FLUIDSYNTH
#include <fluidsynth.h>
#else
#include "i_module.h"
extern FModule FluidSynthModule;

struct fluid_settings_t;
struct fluid_synth_t;
#endif

class FluidSynthMIDIDevice : public SoftSynthMIDIDevice
{
public:
	FluidSynthMIDIDevice(const char *args);
	~FluidSynthMIDIDevice();

	int Open(void (*callback)(unsigned int, void *, DWORD, DWORD), void *userdata);
	FString GetStats();
	void FluidSettingInt(const char *setting, int value);
	void FluidSettingNum(const char *setting, double value);
	void FluidSettingStr(const char *setting, const char *value);

protected:
	void HandleEvent(int status, int parm1, int parm2);
	void HandleLongEvent(const BYTE *data, int len);
	void ComputeOutput(float *buffer, int len);
	int LoadPatchSets(const char *patches);

	fluid_settings_t *FluidSettings;
	fluid_synth_t *FluidSynth;

#ifdef DYN_FLUIDSYNTH
	enum { FLUID_FAILED = -1, FLUID_OK = 0 };
	static TReqProc<FluidSynthModule, fluid_settings_t *(*)()> new_fluid_settings;
	static TReqProc<FluidSynthModule, fluid_synth_t *(*)(fluid_settings_t *)> new_fluid_synth;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *)> delete_fluid_synth;
	static TReqProc<FluidSynthModule, void (*)(fluid_settings_t *)> delete_fluid_settings;
	static TReqProc<FluidSynthModule, int (*)(fluid_settings_t *, const char *, double)> fluid_settings_setnum;
	static TReqProc<FluidSynthModule, int (*)(fluid_settings_t *, const char *, const char *)> fluid_settings_setstr;
	static TReqProc<FluidSynthModule, int (*)(fluid_settings_t *, const char *, int)> fluid_settings_setint;
	static TReqProc<FluidSynthModule, int (*)(fluid_settings_t *, const char *, char **)> fluid_settings_getstr;
	static TReqProc<FluidSynthModule, int (*)(fluid_settings_t *, const char *, int *)> fluid_settings_getint;
	static TReqProc<FluidSynthModule, void (*)(fluid_synth_t *, int)> fluid_synth_set_reverb_on;
	static TReqProc<FluidSynthModule, void (*)(fluid_synth_t *, int)> fluid_synth_set_chorus_on;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int)> fluid_synth_set_interp_method;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int)> fluid_synth_set_polyphony;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *)> fluid_synth_get_polyphony;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *)> fluid_synth_get_active_voice_count;
	static TReqProc<FluidSynthModule, double (*)(fluid_synth_t *)> fluid_synth_get_cpu_load;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *)> fluid_synth_system_reset;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int, int)> fluid_synth_noteon;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int)> fluid_synth_noteoff;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int, int)> fluid_synth_cc;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int)> fluid_synth_program_change;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int)> fluid_synth_channel_pressure;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, int)> fluid_synth_pitch_bend;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, int, void *, int, int, void *, int, int)> fluid_synth_write_float;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, const char *, int)> fluid_synth_sfload;
	static TReqProc<FluidSynthModule, void (*)(fluid_synth_t *, double, double, double, double)> fluid_synth_set_reverb;
	static TReqProc<FluidSynthModule, void (*)(fluid_synth_t *, int, double, double, double, int)> fluid_synth_set_chorus;
	static TReqProc<FluidSynthModule, int (*)(fluid_synth_t *, const char *, int, char *, int *, int *, int)> fluid_synth_sysex;

	bool LoadFluidSynth();
	void UnloadFluidSynth();
#endif
};
#endif

// Base class for streaming MUS and MIDI files ------------------------------

class MIDIStreamer : public MusInfo
{
public:
	MIDIStreamer(EMidiDevice type, const char *args);
	~MIDIStreamer();

	void MusicVolumeChanged();
	void TimidityVolumeChanged();
	void Play(bool looping, int subsong);
	void Pause();
	void Resume();
	void Stop();
	bool IsPlaying();
	bool IsMIDI() const;
	bool IsValid() const;
	bool SetSubsong(int subsong);
	void Update();
	FString GetStats();
	void FluidSettingInt(const char *setting, int value);
	void FluidSettingNum(const char *setting, double value);
	void FluidSettingStr(const char *setting, const char *value);
	void WildMidiSetOption(int opt, int set);
	void CreateSMF(TArray<BYTE> &file, int looplimit=0);

protected:
	MIDIStreamer(const char *dumpname, EMidiDevice type);

	void OutputVolume (DWORD volume);
	int FillBuffer(int buffer_num, int max_events, DWORD max_time);
	int FillStopBuffer(int buffer_num);
	DWORD *WriteStopNotes(DWORD *events);
	int ServiceEvent();
	int VolumeControllerChange(int channel, int volume);
	int ClampLoopCount(int loopcount);
	void SetTempo(int new_tempo);
	static EMidiDevice SelectMIDIDevice(EMidiDevice devtype);
	MIDIDevice *CreateMIDIDevice(EMidiDevice devtype) const;

	static void Callback(unsigned int uMsg, void *userdata, DWORD dwParam1, DWORD dwParam2);

	// Virtuals for subclasses to override
	virtual void StartPlayback();
	virtual void CheckCaps(int tech);
	virtual void DoInitialSetup() = 0;
	virtual void DoRestart() = 0;
	virtual bool CheckDone() = 0;
	virtual void Precache();
	virtual bool SetMIDISubsong(int subsong);
	virtual DWORD *MakeEvents(DWORD *events, DWORD *max_event_p, DWORD max_time) = 0;

	enum
	{
		MAX_EVENTS = 128
	};

	enum
	{
		SONG_MORE,
		SONG_DONE,
		SONG_ERROR
	};

#ifdef _WIN32
	static DWORD WINAPI PlayerProc (LPVOID lpParameter);
	DWORD PlayerLoop();
	
	HANDLE PlayerThread;
	HANDLE ExitEvent;
	HANDLE BufferDoneEvent;
#endif

	MIDIDevice *MIDI;
	DWORD Events[2][MAX_EVENTS*3];
	MIDIHDR Buffer[2];
	int BufferNum;
	int EndQueued;
	bool VolumeChanged;
	bool Restarting;
	bool InitialPlayback;
	DWORD NewVolume;
	int Division;
	int Tempo;
	int InitialTempo;
	BYTE ChannelVolumes[16];
	DWORD Volume;
	EMidiDevice DeviceType;
	bool CallbackIsThreaded;
	int LoopLimit;
	FString DumpFilename;
	FString Args;
};

// MUS file played with a MIDI stream ---------------------------------------

class MUSSong2 : public MIDIStreamer
{
public:
	MUSSong2(FileReader &reader, EMidiDevice type, const char *args);
	~MUSSong2();

	MusInfo *GetWaveDumper(const char *filename, int rate);

protected:
	MUSSong2(const MUSSong2 *original, const char *filename, EMidiDevice type);	// file dump constructor

	void DoInitialSetup();
	void DoRestart();
	bool CheckDone();
	void Precache();
	DWORD *MakeEvents(DWORD *events, DWORD *max_events_p, DWORD max_time);

	MUSHeader *MusHeader;
	BYTE *MusBuffer;
	BYTE LastVelocity[16];
	size_t MusP, MaxMusP;
};

// MIDI file played with a MIDI stream --------------------------------------

class MIDISong2 : public MIDIStreamer
{
public:
	MIDISong2(FileReader &reader, EMidiDevice type, const char *args);
	~MIDISong2();

	MusInfo *GetWaveDumper(const char *filename, int rate);

protected:
	MIDISong2(const MIDISong2 *original, const char *filename, EMidiDevice type);	// file dump constructor

	void CheckCaps(int tech);
	void DoInitialSetup();
	void DoRestart();
	bool CheckDone();
	DWORD *MakeEvents(DWORD *events, DWORD *max_events_p, DWORD max_time);
	void AdvanceTracks(DWORD time);

	struct TrackInfo;

	void ProcessInitialMetaEvents ();
	DWORD *SendCommand (DWORD *event, TrackInfo *track, DWORD delay, ptrdiff_t room, bool &sysex_noroom);
	TrackInfo *FindNextDue ();

	BYTE *MusHeader;
	int SongLen;
	TrackInfo *Tracks;
	TrackInfo *TrackDue;
	int NumTracks;
	int Format;
	uint16_t DesignationMask;
};

// HMI file played with a MIDI stream ---------------------------------------

struct AutoNoteOff
{
	DWORD Delay;
	BYTE Channel, Key;
};
// Sorry, std::priority_queue, but I want to be able to modify the contents of the heap.
class NoteOffQueue : public TArray<AutoNoteOff>
{
public:
	void AddNoteOff(DWORD delay, BYTE channel, BYTE key);
	void AdvanceTime(DWORD time);
	bool Pop(AutoNoteOff &item);

protected:
	void Heapify();

	unsigned int Parent(unsigned int i) const { return (i + 1u) / 2u - 1u; }
	unsigned int Left(unsigned int i) const { return (i + 1u) * 2u - 1u; }
	unsigned int Right(unsigned int i) const { return (i + 1u) * 2u; }
};

class HMISong : public MIDIStreamer
{
public:
	HMISong(FileReader &reader, EMidiDevice type, const char *args);
	~HMISong();

	MusInfo *GetWaveDumper(const char *filename, int rate);

protected:
	HMISong(const HMISong *original, const char *filename, EMidiDevice type);	// file dump constructor

	void SetupForHMI(int len);
	void SetupForHMP(int len);
	void CheckCaps(int tech);

	void DoInitialSetup();
	void DoRestart();
	bool CheckDone();
	DWORD *MakeEvents(DWORD *events, DWORD *max_events_p, DWORD max_time);
	void AdvanceTracks(DWORD time);

	struct TrackInfo;

	void ProcessInitialMetaEvents ();
	DWORD *SendCommand (DWORD *event, TrackInfo *track, DWORD delay, ptrdiff_t room, bool &sysex_noroom);
	TrackInfo *FindNextDue ();

	static DWORD ReadVarLenHMI(TrackInfo *);
	static DWORD ReadVarLenHMP(TrackInfo *);

	BYTE *MusHeader;
	int SongLen;
	int NumTracks;
	TrackInfo *Tracks;
	TrackInfo *TrackDue;
	TrackInfo *FakeTrack;
	DWORD (*ReadVarLen)(TrackInfo *);
	NoteOffQueue NoteOffs;
};

// XMI file played with a MIDI stream ---------------------------------------

class XMISong : public MIDIStreamer
{
public:
	XMISong(FileReader &reader, EMidiDevice type, const char *args);
	~XMISong();

	MusInfo *GetWaveDumper(const char *filename, int rate);

protected:
	struct TrackInfo;
	enum EventSource { EVENT_None, EVENT_Real, EVENT_Fake };

	XMISong(const XMISong *original, const char *filename, EMidiDevice type);	// file dump constructor

	int FindXMIDforms(const BYTE *chunk, int len, TrackInfo *songs) const;
	void FoundXMID(const BYTE *chunk, int len, TrackInfo *song) const;
	bool SetMIDISubsong(int subsong);
	void DoInitialSetup();
	void DoRestart();
	bool CheckDone();
	DWORD *MakeEvents(DWORD *events, DWORD *max_events_p, DWORD max_time);
	void AdvanceSong(DWORD time);

	void ProcessInitialMetaEvents();
	DWORD *SendCommand (DWORD *event, EventSource track, DWORD delay, ptrdiff_t room, bool &sysex_noroom);
	EventSource FindNextDue();

	BYTE *MusHeader;
	int SongLen;		// length of the entire file
	int NumSongs;
	TrackInfo *Songs;
	TrackInfo *CurrSong;
	NoteOffQueue NoteOffs;
	EventSource EventDue;
};

// Anything supported by the sound system out of the box --------------------

class StreamSong : public MusInfo
{
public:
    StreamSong (FileReader *reader);
	StreamSong (const char *url);
	~StreamSong ();
	void Play (bool looping, int subsong);
	void Pause ();
	void Resume ();
	void Stop ();
	bool IsPlaying ();
	bool IsValid () const { return m_Stream != NULL; }
	bool SetPosition (unsigned int pos);
	bool SetSubsong (int subsong);
	FString GetStats();

protected:
	StreamSong () : m_Stream(NULL) {}

	SoundStream *m_Stream;
};

// CD track/disk played through the multimedia system -----------------------

class CDSong : public MusInfo
{
public:
	CDSong (int track, int id);
	~CDSong ();
	void Play (bool looping, int subsong);
	void Pause ();
	void Resume ();
	void Stop ();
	bool IsPlaying ();
	bool IsValid () const { return m_Inited; }

protected:
	CDSong () : m_Inited(false) {}

	int m_Track;
	bool m_Inited;
};

// CD track on a specific disk played through the multimedia system ---------

class CDDAFile : public CDSong
{
public:
	CDDAFile (FileReader &reader);
};

// Module played via foo_dumb -----------------------------------------------

MusInfo *MOD_OpenSong(FileReader &reader);

// Music played via Game Music Emu ------------------------------------------

const char *GME_CheckFormat(uint32 header);
MusInfo *GME_OpenSong(FileReader &reader, const char *fmt);

// --------------------------------------------------------------------------

extern MusInfo *currSong;

EXTERN_CVAR (Float, snd_musicvolume)
