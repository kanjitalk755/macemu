/*
 *  cinepak_hooks.h - Native Cinepak decoder registered as a QuickTime
 *                    image decompressor ('imdc'/'cvid') component
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

#ifndef CINEPAK_HOOKS_H
#define CINEPAK_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Install first-instruction hooks on InterfaceLib's OpenDefaultComponent and
 * FindNextComponent. When any caller (QuickTime's ICM included) searches for
 * an image decompressor, the hook registers our native 'imdc'/'cvid'
 * component just-in-time - newest registration is found first - then calls
 * the real function. Once registered, both hooks restore themselves.
 * Called from InitCallUniversalProc (same timing as the InterfaceLib
 * Microseconds patch). Idempotent. */
extern bool CinepakInstallHooks(void);

/* Register the native 'imdc'/'cvid' component. MUST be called from
 * native-op context (e.g. VideoInstallAccel), NOT from an EMUL_OP handler:
 * it invokes RegisterComponent via call_macos6, which wedges from
 * MODE_EMUL_OP. Idempotent; returns true once registered. */
extern bool CinepakRegisterFromNative(void);

/* Clear the registration latch for a guest soft reboot so the decoder
 * re-registers into the fresh Component Manager (see GfxAccelResetForReboot). */
extern void CinepakResetForReboot(void);

/* Native op handlers for the patched entry points (FN=1: they ARE the whole
 * function; original args arrive untouched in r3/r4). */
extern uint32 CinepakOpenDefaultComponentHook(uint32 componentType,
	uint32 componentSubType);
extern uint32 CinepakFindNextComponentHook(uint32 aComponent,
	uint32 lookingDesc);

/* Component entry point: pascal ComponentResult (ComponentParameters *cp,
 * Handle storage). MixedMode marshals cp into r3 via the routine
 * descriptor's procInfo. */
extern uint32 CinepakDispatch(uint32 componentParameters, int svq1);

#ifdef __cplusplus
}
#endif

#endif /* CINEPAK_HOOKS_H */
