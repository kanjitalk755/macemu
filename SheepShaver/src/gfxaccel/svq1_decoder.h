/*
 *  svq1_decoder.h - Sorenson Video 1 (SVQ1) decoder
 * 
 *	(C) 2026 RandoOnSteam (battlemageloveryt@gmail.com)
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

#ifndef SVQ1_DECODER_H
#define SVQ1_DECODER_H

typedef struct Svq1Decoder Svq1Decoder;

Svq1Decoder *Svq1Create(int width, int height);
void Svq1Destroy(Svq1Decoder *decoder);
int Svq1Decode(Svq1Decoder *decoder, const uint8 *data, uint32 length);
int Svq1Width(Svq1Decoder *decoder);
int Svq1Height(Svq1Decoder *decoder);
const uint8 *Svq1Plane(Svq1Decoder *decoder, int plane, int *pitch);
const uint32 *Svq1Frame(Svq1Decoder *decoder);

#endif
