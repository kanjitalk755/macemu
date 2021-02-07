/*
 *  startupchime.cpp - SDL code to play the startup chime
 *
 *  Basilisk II (C) Christian Bauer
 *
 *  Created by Richard Cini on 2/5/21.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "startupchime.hpp"
# include <SDL.h>
# include <SDL_main.h>


// Plays a startup chime saved as a 44100kHz mono WAV file. The sample is saved in
// the same folder as the program (not as a package resource) to make it easily
// swappable. If the file isn't found in the folder, the routine just exits.

void PlayStartupChime(void){
    SDL_AudioSpec wav_spec;
    Uint32 wav_length;
    Uint8 *wav_buffer;
    SDL_AudioSpec desired;
    SDL_AudioSpec obtained;

 
    SDL_zero(desired);
    desired.freq = 44100;
    desired.format = AUDIO_S16;
    desired.channels = 1;
    desired.samples = 4096;
    desired.callback = NULL;

    if (SDL_LoadWAV("startup.wav", &wav_spec, &wav_buffer, &wav_length)){
        SDL_AudioDeviceID deviceId = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);

        if (deviceId){
            int success = SDL_QueueAudio(deviceId, wav_buffer, wav_length);
            SDL_PauseAudioDevice(deviceId, 0);
            SDL_Delay(1500);
            SDL_CloseAudioDevice(deviceId);
        }
        else {
            printf("%s", "Audio driver failed to initialize");
        }
        SDL_FreeWAV(wav_buffer);
    }
    else {
        printf("%s", "WAV file failed to load");
    }
}
