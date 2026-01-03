/*
 *  audio_ipc.h - IPC-based audio driver header
 */

#ifndef AUDIO_IPC_H
#define AUDIO_IPC_H

#ifdef ENABLE_IPC_AUDIO

// Audio functions exported to audio.cpp
extern void AudioInit(void);
extern void AudioExit(void);
extern void audio_enter_stream(void);
extern void audio_exit_stream(void);
extern void AudioInterrupt(void);

extern bool audio_get_main_mute(void);
extern uint32 audio_get_main_volume(void);
extern bool audio_get_speaker_mute(void);
extern uint32 audio_get_speaker_volume(void);

extern void audio_set_main_mute(bool mute);
extern void audio_set_main_volume(uint32 vol);
extern void audio_set_speaker_mute(bool mute);
extern void audio_set_speaker_volume(uint32 vol);

extern bool audio_set_sample_rate(int index);
extern bool audio_set_sample_size(int index);
extern bool audio_set_channels(int index);

// Pull model: Server requests audio data
extern void audio_request_data(uint32 requested_samples);

#endif // ENABLE_IPC_AUDIO

#endif // AUDIO_IPC_H
