/*
 *  cinepak_hooks.cpp - Native Cinepak decoder registered as a QuickTime
 *                      image decompressor ('imdc'/'cvid') component
 *
 *  Strategy
 *  --------
 *  QuickTime's Image Compression Manager picks a decompressor for 'cvid'
 *  frames through the Component Manager. We register a real 'imdc'/'cvid'
 *  component whose entry point is a SheepShaver routine descriptor that
 *  lands in CinepakDispatch() - decode happens in host code, eliminating
 *  both the one-time codec setup stall and the per-frame interpreter cost.
 *
 *  Registration timing is the crux: our component must be the one the ICM
 *  finds FIRST, and the Component Manager returns the most recently
 *  registered match first. So we register just-in-time: first-instruction
 *  hooks (FN=1 native ops, the InterfaceLib-Microseconds pattern) on
 *  OpenDefaultComponent and FindNextComponent watch for the first 'imdc'
 *  search, register our component right then - before the search executes -
 *  and permanently restore both patch sites once registered.
 *
 *  The component implements the CLASSIC codec protocol (codecGetCodecInfo /
 *  codecPreDecompress / codecBandDecompress), which every QuickTime version
 *  services. If anything looks unsupported we return noCodecErr, and the
 *  ICM falls back to Apple's Cinepak - the movie still plays, just slow.
 */

#include "sysdeps.h"
#include "cpu_emulation.h"
#include "macos_util.h"
#include "thunks.h"
#include "main.h"
#include "cinepak_decoder.h"
#include "cinepak_hooks.h"
#include "gfx_log.h"

#define DEBUG 0
#include "debug.h"
#include <stdio.h>
#include <string.h>

/* Bring-up diagnostics. Default OFF for the shipping build; flip to 1 to
 * trace registration, codec negotiation, blit geometry, and per-frame
 * cadence (the last verified the ~0.85s first-frame stall is gone - steady
 * ~68ms/frame, zero >120ms gaps). */
#ifndef CINEPAK_LOGGING_ENABLED
#define CINEPAK_LOGGING_ENABLED 1
#endif

#if CINEPAK_LOGGING_ENABLED
#include "gfx_log.h"

/* Route through the shared stderr + OutputDebugStringA sink. Keeps the
 * "[CINEPAK:...]" text so existing greps/notes still match. The source lines
 * pass their own trailing "\n"; wrap so the body sits inside the brackets. */
static inline void CINEPAK_LOG(const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';
	::gfx_debug::emit("[CINEPAK:", "%s]", message);
}
#else
#define CINEPAK_LOG(...) do { } while (0)
#endif

#if EMULATED_PPC

static inline uint32 MAKE_FOURCC(char a, char b, char c, char d)
{
	return ((uint32)(uint8)a << 24) |
	       ((uint32)(uint8)b << 16) |
	       ((uint32)(uint8)c << 8)  |
	       (uint32)(uint8)d;
}

#define FOURCC_IMDC MAKE_FOURCC('i','m','d','c')
#define FOURCC_CVID MAKE_FOURCC('c','v','i','d')
#define FOURCC_APPL MAKE_FOURCC('a','p','p','l')

/* pascal ComponentResult (*)(ComponentParameters *, Handle):
 * kPascalStackBased | RESULT_SIZE(4) | PARAM(1,4) | PARAM(2,4) */
static const uint32 kComponentRoutineProcInfo = 0x000003F0;

/* Component Manager standard selectors. */
enum {
	kComponentOpenSelect       = -1,
	kComponentCloseSelect      = -2,
	kComponentCanDoSelect      = -3,
	kComponentVersionSelect    = -4,
	kComponentRegisterSelect   = -5,
	kComponentTargetSelect     = -6,
	kComponentUnregisterSelect = -7
};

/* Classic codec-component selectors (Inside Mac: QuickTime Components). */
enum {
	codecGetCodecInfo   = 0,
	codecPreDecompress  = 5,
	codecBandDecompress = 6,
	codecBusy           = 7
};

/* OSErr / ComponentResult values. */
static const uint32 kNoErr                = 0;
static const uint32 kParamErr             = (uint32)-50;
static const uint32 kNoCodecErr           = (uint32)-8961;
static const uint32 kCodecUnimpErr        = (uint32)-8962;
static const uint32 kBadComponentSelector = 0x80008002;

/* CodecDecompressParams offsets (68k 2-byte alignment). */
static const int O_P_SEQUENCE_ID = 0;
static const int O_P_IMAGE_DESC  = 4;   /* ImageDescriptionHandle */
static const int O_P_DATA        = 8;
static const int O_P_BUFFER_SIZE = 12;
static const int O_P_FRAME_NUM   = 16;
static const int O_P_START_LINE  = 20;
static const int O_P_STOP_LINE   = 24;
static const int O_P_COND_FLAGS  = 28;
static const int O_P_CAPABILITIES = 34; /* CodecCapabilities *              */
static const int O_P_DST_PIXMAP  = 66;  /* PixMap (50 bytes), inline        */
/* Fields after the inline dstPixMap (classic ImageCodec.h layout). */
static const int O_P_SRC_RECT    = 124; /* Rect  srcRect (image sub-rect)   */
static const int O_P_MATRIX      = 132; /* MatrixRecord * (image->dst xform)*/
static const int O_P_TRANSFERMODE = 140;

/* PixMap offsets relative to O_P_DST_PIXMAP. */
static const int O_PM_BASE_ADDR  = 0;
static const int O_PM_ROW_BYTES  = 4;
static const int O_PM_BOUNDS     = 6;   /* top,left,bottom,right int16      */
static const int O_PM_PIXEL_SIZE = 32;
static const int O_PM_CMP_COUNT  = 34;

/* ImageDescription offsets (dereferenced handle). */
static const int O_ID_CTYPE  = 4;
static const int O_ID_WIDTH  = 32;
static const int O_ID_HEIGHT = 34;
static const int O_ID_DEPTH  = 82;

/* CodecCapabilities offsets. */
static const int O_CAP_FLAGS         = 0;
static const int O_CAP_WANTED_PIXSZ  = 4;
static const int O_CAP_EXTEND_WIDTH  = 6;
static const int O_CAP_EXTEND_HEIGHT = 8;
static const int O_CAP_BAND_MIN      = 10;
static const int O_CAP_BAND_INC      = 12;
static const int O_CAP_PAD           = 14;
static const int O_CAP_TIME          = 16;

static inline int16 read_mac_int16(uint32 addr)
{
	return (int16)ReadMacInt16(addr);
}

/* ----------------------------------------------------------------------
 *  Just-in-time component registration
 * ---------------------------------------------------------------------- */

static bool s_registered = false;
static bool s_register_in_progress = false;
static SheepRoutineDescriptor *s_entry_rd = NULL;

struct HookSite {
	const char *name;
	int native_sel;
	uint32 tvect;
	uint32 code;
	uint32 orig_insn;
	bool patched;
};

static HookSite s_odc = { "OpenDefaultComponent",
	NATIVE_OPENDEFAULTCOMPONENT_CINEPAK_HOOK, 0, 0, 0, false };
static HookSite s_fnc = { "FindNextComponent",
	NATIVE_FINDNEXTCOMPONENT_CINEPAK_HOOK, 0, 0, 0, false };

static void uninstall_site(HookSite &s)
{
	if (!s.patched)
		return;
	WriteMacInt32(s.code, s.orig_insn);
	FlushCodeCache(s.code, s.code + 4);
	s.patched = false;
	CINEPAK_LOG("hook on %s restored\n", s.name);
}

/* Call the real function while the site is temporarily restored. Safe for
 * nested searches: a miss during the window is benign (the hook re-arms
 * right after), and registration itself is guarded separately. */
static uint32 call_original2(HookSite &s, uint32 arg1, uint32 arg2)
{
	if (!s.patched)
		return call_macos2(s.tvect, arg1, arg2);

	WriteMacInt32(s.code, s.orig_insn);
	FlushCodeCache(s.code, s.code + 4);
	uint32 result = call_macos2(s.tvect, arg1, arg2);
	if (s.patched) {
		WriteMacInt32(s.code, NativeOpcode(s.native_sel));
		FlushCodeCache(s.code, s.code + 4);
	}
	return result;
}

/* Register our 'imdc'/'cvid' component. Runs from native-op context, where
 * call_macos into InterfaceLib is the established pattern (DSpInstallHooks /
 * FindLibSymbol do the same). Once registered, the trigger hooks restore
 * themselves - zero steady-state overhead. */
static bool CinepakEnsureRegistered(void)
{
	if (s_registered)
		return true;
	if (s_register_in_progress)
		return false;
	s_register_in_progress = true;

	uint32 reg_tvect = FindLibSymbol("\014InterfaceLib", "\021RegisterComponent");
	if (reg_tvect == 0) {
		CINEPAK_LOG("RegisterComponent not found in InterfaceLib\n");
		s_register_in_progress = false;
		return false;
	}

	if (s_entry_rd == NULL)
		s_entry_rd = new SheepRoutineDescriptor(kComponentRoutineProcInfo,
		                                        NativeTVECT(NATIVE_CINEPAK_DISPATCH));

	SheepVar cd_var(20);
	const uint32 cd = cd_var.addr();
	WriteMacInt32(cd + 0,  FOURCC_IMDC);  // componentType
	WriteMacInt32(cd + 4,  FOURCC_CVID);  // componentSubType
	WriteMacInt32(cd + 8,  FOURCC_APPL);  // componentManufacturer
	WriteMacInt32(cd + 12, 0);            // componentFlags
	WriteMacInt32(cd + 16, 0);            // componentFlagsMask

	/* RegisterComponent(cd, entryPoint, global, name, info, icon) */
	uint32 comp = call_macos6(reg_tvect, cd, s_entry_rd->addr(), 1, 0, 0, 0);
	s_register_in_progress = false;

	if (comp == 0) {
		CINEPAK_LOG("RegisterComponent FAILED (returned 0)\n");
		return false;
	}

	CINEPAK_LOG("native 'imdc'/'cvid' component registered: %08x (entry RD %08x)\n",
	            comp, s_entry_rd->addr());
	s_registered = true;

	/* Triggers are no longer needed. */
	uninstall_site(s_odc);
	uninstall_site(s_fnc);
	return true;
}

/* ----------------------------------------------------------------------
 *  Trigger hooks (FN=1 native ops on the first instruction; the handler IS
 *  the function and returns via LR)
 * ---------------------------------------------------------------------- */

uint32 CinepakOpenDefaultComponentHook(uint32 componentType, uint32 componentSubType)
{
	if (componentType == FOURCC_IMDC) {
		CINEPAK_LOG("OpenDefaultComponent('%c%c%c%c','%c%c%c%c') - registering\n",
		            (char)(componentType >> 24), (char)(componentType >> 16),
		            (char)(componentType >> 8), (char)componentType,
		            (char)(componentSubType >> 24), (char)(componentSubType >> 16),
		            (char)(componentSubType >> 8), (char)componentSubType);
		CinepakEnsureRegistered();
	}
	return call_original2(s_odc, componentType, componentSubType);
}

uint32 CinepakFindNextComponentHook(uint32 aComponent, uint32 lookingDesc)
{
	if (lookingDesc != 0) {
		const uint32 type = ReadMacInt32(lookingDesc + 0);
		const uint32 sub  = ReadMacInt32(lookingDesc + 4);
		if (type == FOURCC_IMDC || sub == FOURCC_CVID) {
			static int logged = 0;
			if (logged < 8) {
				logged++;
				CINEPAK_LOG("FindNextComponent(type='%c%c%c%c' sub='%c%c%c%c') - registering\n",
				            (char)(type >> 24), (char)(type >> 16),
				            (char)(type >> 8), (char)type,
				            (char)(sub >> 24), (char)(sub >> 16),
				            (char)(sub >> 8), (char)sub);
			}
			CinepakEnsureRegistered();
		}
	}
	return call_original2(s_fnc, aComponent, lookingDesc);
}

static bool install_site(HookSite &s, const char *pascal_sym)
{
	if (s.patched)
		return true;
	s.tvect = FindLibSymbol("\014InterfaceLib", pascal_sym);
	if (s.tvect == 0) {
		CINEPAK_LOG("%s not found in InterfaceLib\n", s.name);
		return false;
	}
	s.code = ReadMacInt32(s.tvect);
	if (s.code == 0) {
		CINEPAK_LOG("%s has a null code pointer\n", s.name);
		s.tvect = 0;
		return false;
	}
	s.orig_insn = ReadMacInt32(s.code);
	WriteMacInt32(s.code, NativeOpcode(s.native_sel));
	FlushCodeCache(s.code, s.code + 4);
	s.patched = true;
	CINEPAK_LOG("hook installed on %s at %08x (orig insn %08x)\n",
	            s.name, s.code, s.orig_insn);
	return true;
}

bool CinepakInstallHooks(void)
{
	/* Install the search-site hooks only. Registration itself must NOT run
	 * here: this is called from InitCallUniversalProc, which executes in
	 * MODE_EMUL_OP, and RegisterComponent starts a nested guest execution
	 * (call_macos6) that wedges from EMUL_OP context - observed as an
	 * "Illegal instruction" at boot. Registration is deferred to
	 * CinepakRegisterFromNative(), driven from VideoInstallAccel (native-op
	 * context, the same path DSp/RAVE register from safely).
	 *
	 * Pascal length prefixes: "OpenDefaultComponent" = 20 = \024,
	 * "FindNextComponent" = 17 = \021. */
	const bool odc = install_site(s_odc, "\024OpenDefaultComponent");
	const bool fnc = install_site(s_fnc, "\021FindNextComponent");
	return odc || fnc;
}

/* Native-op-context registration entry, called from VideoInstallAccel on
 * each accRun tick until it succeeds. Safe to call repeatedly (idempotent
 * via s_registered). Registering proactively - rather than only on a caught
 * search - is what actually gets us picked: QuickTime's ICM resolves its
 * codec through a bound FindNextComponent that never hits our InterfaceLib
 * patch sites (observed: the hooks install but never fire). Newest
 * registration wins, so as long as we register before QuickTime enumerates
 * codecs for the first movie, our decoder is selected. */
bool CinepakRegisterFromNative(void)
{
	return CinepakEnsureRegistered();
}

/* Clear the registration latch for a guest soft reboot. The guest Component
 * Manager is reset (our 'imdc'/'cvid' registration is gone) and the trap sites
 * we hook reload fresh, so re-registration must run again. InitCallUniversalProc
 * re-arms the search-site hooks on the fresh boot, and VideoInstallAccel's
 * CinepakRegisterFromNative re-registers once flags are cleared here. The host
 * RoutineDescriptor (s_entry_rd) is process-persistent and reused. */
void CinepakResetForReboot(void)
{
	CINEPAK_LOG("CinepakResetForReboot: registered=%d\n", s_registered);
	uninstall_site(s_odc);
	uninstall_site(s_fnc);
	s_registered = false;
	s_register_in_progress = false;
}

/* ----------------------------------------------------------------------
 *  Per-sequence decoder contexts
 *
 *  Cinepak codebooks and the previous frame persist across frames, so each
 *  ImageSequence gets one long-lived host context. Small LRU table - a
 *  movie uses one or two sequences.
 * ---------------------------------------------------------------------- */

struct SeqSlot {
	uint32 seq_id;
	cinepak_context_t *ctx;
	uint32 stamp;
	bool in_use;
};

static SeqSlot s_slots[8];
static uint32 s_lru_stamp = 0;

static cinepak_context_t *get_context(uint32 seq_id, int width, int height)
{
	SeqSlot *victim = NULL;
	for (int i = 0; i < 8; i++) {
		SeqSlot &sl = s_slots[i];
		if (sl.in_use && sl.seq_id == seq_id) {
			if (cinepak_width(sl.ctx) != width || cinepak_height(sl.ctx) != height) {
				cinepak_destroy(sl.ctx);
				sl.ctx = cinepak_create(width, height);
			}
			sl.stamp = ++s_lru_stamp;
			return sl.ctx;
		}
		if (victim == NULL || !sl.in_use ||
		    (victim->in_use && sl.stamp < victim->stamp))
			victim = &sl;
	}
	if (victim->in_use && victim->ctx)
		cinepak_destroy(victim->ctx);
	victim->seq_id = seq_id;
	victim->ctx = cinepak_create(width, height);
	victim->stamp = ++s_lru_stamp;
	victim->in_use = (victim->ctx != NULL);
	return victim->ctx;
}

/* ----------------------------------------------------------------------
 *  Component protocol
 * ---------------------------------------------------------------------- */

static void fill_codec_info(uint32 info)
{
	static const char name[] = "Cinepak";
	const int name_len = (int)(sizeof(name) - 1);
	WriteMacInt8(info + 0, name_len);
	for (int i = 0; i < 31; i++)
		WriteMacInt8(info + 1 + i, i < name_len ? name[i] : 0);

	WriteMacInt16(info + 32, 0x0002);       // version
	WriteMacInt16(info + 34, 0x0001);       // revisionLevel
	WriteMacInt32(info + 36, FOURCC_APPL);  // vendor
	/* decompressFlags: codecInfoDoes16 | codecInfoDoes32 | codecInfoDoesTemporal */
	WriteMacInt32(info + 40, 0x00000430);
	WriteMacInt32(info + 44, 0);            // compressFlags (decode-only)
	/* formatFlags: codecInfoDepth24 | codecInfoSequenceSensitive */
	WriteMacInt32(info + 48, 0x00002040);
	WriteMacInt8 (info + 52, 128);          // compressionAccuracy
	WriteMacInt8 (info + 53, 128);          // decompressionAccuracy
	WriteMacInt16(info + 54, 200);          // compressionSpeed (ms)
	WriteMacInt16(info + 56, 100);          // decompressionSpeed (ms)
	WriteMacInt8 (info + 58, 128);          // compressionLevel
	WriteMacInt8 (info + 59, 0);            // resvd
	WriteMacInt16(info + 60, 1);            // minimumHeight
	WriteMacInt16(info + 62, 1);            // minimumWidth
	WriteMacInt16(info + 64, 0);            // decompressPipelineLatency
	WriteMacInt16(info + 66, 0);            // compressPipelineLatency
	WriteMacInt32(info + 68, 0);            // privateData
}

#if CINEPAK_LOGGING_ENABLED
static void dump_params_once(const char *tag, uint32 p)
{
	static int dumped = 0;
	if (dumped >= 2)
		return;
	dumped++;
	char line[3 * 16 + 16];
	CINEPAK_LOG("%s params @%08x:\n", tag, p);
	for (int off = 0; off < 0x90; off += 16) {
		int n = 0;
		for (int i = 0; i < 16; i++)
			n += snprintf(line + n, sizeof(line) - n, "%02x ",
			              (unsigned)ReadMacInt8(p + off + i));
		CINEPAK_LOG("  +%02x: %s\n", off, line);
	}
}
#else
static void dump_params_once(const char *, uint32) { }
#endif

static uint32 cinepak_pre_decompress(uint32 p)
{
	if (p == 0)
		return kParamErr;
	dump_params_once("PreDecompress", p);

	const uint32 id_h = ReadMacInt32(p + O_P_IMAGE_DESC);
	if (id_h == 0)
		return kParamErr;
	const uint32 id = ReadMacInt32(id_h);
	const uint32 ctype = ReadMacInt32(id + O_ID_CTYPE);
	if (ctype != FOURCC_CVID)
		return kNoCodecErr;

	const int16 width  = read_mac_int16(id + O_ID_WIDTH);
	const int16 height = read_mac_int16(id + O_ID_HEIGHT);
	const int16 dst_pix_size =
		read_mac_int16(p + O_P_DST_PIXMAP + O_PM_PIXEL_SIZE);

	CINEPAK_LOG("PreDecompress: %dx%d depth=%d dstPixelSize=%d\n",
	            (int)width, (int)height,
	            (int)read_mac_int16(id + O_ID_DEPTH), (int)dst_pix_size);

	if (width <= 0 || height <= 0)
		return kNoCodecErr;

	const uint32 caps = ReadMacInt32(p + O_P_CAPABILITIES);
	if (caps != 0) {
		const int16 wanted =
			(dst_pix_size == 32 || dst_pix_size == 16) ? dst_pix_size : 16;
		WriteMacInt32(caps + O_CAP_FLAGS, 0);
		WriteMacInt16(caps + O_CAP_WANTED_PIXSZ, wanted);
		WriteMacInt16(caps + O_CAP_EXTEND_WIDTH, (4 - (width & 3)) & 3);
		WriteMacInt16(caps + O_CAP_EXTEND_HEIGHT, (4 - (height & 3)) & 3);
		WriteMacInt16(caps + O_CAP_BAND_MIN, height);
		WriteMacInt16(caps + O_CAP_BAND_INC, height);
		WriteMacInt16(caps + O_CAP_PAD, 0);
		WriteMacInt32(caps + O_CAP_TIME, 0);
	}
	return kNoErr;
}

static uint32 cinepak_band_decompress(uint32 p)
{
	if (p == 0)
		return kParamErr;
	dump_params_once("BandDecompress", p);

	const uint32 id_h = ReadMacInt32(p + O_P_IMAGE_DESC);
	if (id_h == 0)
		return kParamErr;
	const uint32 id = ReadMacInt32(id_h);
	if (ReadMacInt32(id + O_ID_CTYPE) != FOURCC_CVID)
		return kNoCodecErr;

	const int width  = read_mac_int16(id + O_ID_WIDTH);
	const int height = read_mac_int16(id + O_ID_HEIGHT);
	const uint32 seq_id = ReadMacInt32(p + O_P_SEQUENCE_ID);
	const uint32 data = ReadMacInt32(p + O_P_DATA);
	const uint32 buffer_size = ReadMacInt32(p + O_P_BUFFER_SIZE);

	if (width <= 0 || height <= 0 || data == 0 || buffer_size < 10)
		return kNoCodecErr;

	cinepak_context_t *ctx = get_context(seq_id, width, height);
	if (ctx == NULL)
		return kNoCodecErr;

	const uint8 *src = Mac2HostAddr(data);
	if (cinepak_decode_frame(ctx, src, buffer_size) < 0) {
		CINEPAK_LOG("decode FAILED: seq=%08x frame=%d size=%u\n",
		            seq_id, (int)ReadMacInt32(p + O_P_FRAME_NUM), buffer_size);
		return kNoCodecErr;
	}

#if CINEPAK_LOGGING_ENABLED
	/* Frame cadence probe: log the wall-clock gap (ms) between decodes so a
	 * stall - the ~0.85s first-frame freeze this work set out to kill - shows
	 * as a large delta. GetTicks_usec() is BasiliskII's host microsecond
	 * clock (already used by audio.cpp). */
	{
		static uint64 last_us = 0;
		static int frame_no = 0;
		const uint64 now = GetTicks_usec();
		const double gap_ms = last_us ? (double)(now - last_us) / 1000.0 : 0.0;
		if (frame_no < 60 || gap_ms > 80.0)
			CINEPAK_LOG("decode ok frame=%d gap=%.1fms size=%u%s\n",
			            frame_no, gap_ms, buffer_size,
			            gap_ms > 120.0 ? "  <== STALL" : "");
		last_us = now;
		frame_no++;
	}
#endif

	/* Destination PixMap. */
	const uint32 pm = p + O_P_DST_PIXMAP;
	const uint32 base_addr = ReadMacInt32(pm + O_PM_BASE_ADDR);
	const int row_bytes = ReadMacInt16(pm + O_PM_ROW_BYTES) & 0x3FFF;
	const int16 b_top    = read_mac_int16(pm + O_PM_BOUNDS + 0);
	const int16 b_left   = read_mac_int16(pm + O_PM_BOUNDS + 2);
	const int16 b_bottom = read_mac_int16(pm + O_PM_BOUNDS + 4);
	const int16 b_right  = read_mac_int16(pm + O_PM_BOUNDS + 6);
	const int pixel_size = read_mac_int16(pm + O_PM_PIXEL_SIZE);

	if (base_addr == 0 || row_bytes <= 0)
		return kNoCodecErr;
	if (pixel_size != 16 && pixel_size != 32) {
		CINEPAK_LOG("unsupported dst pixelSize=%d (negotiation should have "
		            "prevented this)\n", pixel_size);
		return kNoCodecErr;
	}

	/* Band rows: baseAddr points at the band's first row, which is image
	 * row startLine. With bandMin = frame height this is the whole frame. */
	int32 start_line = (int32)ReadMacInt32(p + O_P_START_LINE);
	int32 stop_line  = (int32)ReadMacInt32(p + O_P_STOP_LINE);
	if (start_line < 0 || start_line >= height)
		start_line = 0;
	if (stop_line <= start_line || stop_line > height)
		stop_line = height;

	/* Destination placement.
	 *
	 * baseAddr addresses the pixel at (bounds.top, bounds.left) of the
	 * destination pixmap. The image is mapped into destination space by the
	 * decompress matrix; for Cinepak playback this is (observed) always an
	 * identity/translate matrix - scale is 1 and rotation absent - so we
	 * honor just the integer translation and clip. Image pixel (ix,iy)
	 * therefore lands at destination pixel (iy+ty, ix+tx), which is buffer
	 * offset (iy+ty - bounds.top, ix+tx - bounds.left).
	 *
	 * The earlier "half size / misplaced" symptom was writing from baseAddr
	 * with no bounds offset, so the image sat at the buffer's physical
	 * top-left (pixel (-84,-24) in dst space) instead of at dst (0,0). */
	const uint32 matrix = ReadMacInt32(p + O_P_MATRIX);
	int tx = 0, ty = 0;
	if (matrix != 0) {
		tx = (int)(((int32)ReadMacInt32(matrix + 24)) >> 16); /* [2][0] */
		ty = (int)(((int32)ReadMacInt32(matrix + 28)) >> 16); /* [2][1] */
	}

	/* srcRect selects the sub-region of the image to draw (full image here).
	 * Rect is top,left,bottom,right. */
	int src_top    = read_mac_int16(p + O_P_SRC_RECT + 0);
	int src_left   = read_mac_int16(p + O_P_SRC_RECT + 2);
	int src_bottom = read_mac_int16(p + O_P_SRC_RECT + 4);
	int src_right  = read_mac_int16(p + O_P_SRC_RECT + 6);
	if (src_right <= src_left || src_bottom <= src_top) {
		src_top = 0; src_left = 0; src_bottom = height; src_right = width;
	}
	if (src_right > width)  src_right = width;
	if (src_bottom > height) src_bottom = height;

	/* Destination buffer dimensions from the pixmap bounds. */
	const int dst_w = (int)b_right - (int)b_left;
	const int dst_h = (int)b_bottom - (int)b_top;
	static int blit_logged = 0;
	if (blit_logged < 4) {
		blit_logged++;
		CINEPAK_LOG("blit: seq=%08x %dx%d -> base=%08x rowBytes=%d "
		            "bounds=(%d,%d,%d,%d) pixSize=%d lines=%d..%d key=%d "
		            "matrix=%08x srcRect=(%d,%d,%d,%d)\n",
		            seq_id, width, height, base_addr, row_bytes,
		            (int)b_top, (int)b_left, (int)b_bottom, (int)b_right,
		            pixel_size, start_line, stop_line,
		            cinepak_last_frame_was_key(ctx), matrix,
		            (int)read_mac_int16(p + O_P_SRC_RECT + 0),
		            (int)read_mac_int16(p + O_P_SRC_RECT + 2),
		            (int)read_mac_int16(p + O_P_SRC_RECT + 4),
		            (int)read_mac_int16(p + O_P_SRC_RECT + 6));
		if (matrix != 0) {
			/* MatrixRecord is 9 Fixed (3x3), row-major. Log the diagonal
			 * scale terms [0][0],[1][1] and translation [2][0],[2][1]. */
			CINEPAK_LOG("  matrix sx=%.4f sy=%.4f tx=%.4f ty=%.4f\n",
			            (double)(int32)ReadMacInt32(matrix + 0)  / 65536.0,
			            (double)(int32)ReadMacInt32(matrix + 16) / 65536.0,
			            (double)(int32)ReadMacInt32(matrix + 24) / 65536.0,
			            (double)(int32)ReadMacInt32(matrix + 28) / 65536.0);
		}
	}

	const uint32_t *frame = cinepak_frame(ctx);
	uint8 *dst_base = Mac2HostAddr(base_addr);
#if DESCENT_HITCH_DEBUG
	if (blit_logged <= 4)
		CINEPAK_LOG("blit dstHost=%p (base=%08x) pixSize=%d\n",
		            (void *)dst_base, base_addr, pixel_size);
#endif
	const int bytes_pp = (pixel_size == 32) ? 4 : 2;

	/* WRITE-PROBE instrumentation (DESCENT_HITCH_DEBUG): prove the 16bpp pixel
	 * loop actually runs and mutates the destination, and that the bytes we
	 * write match what the compositor later reads. Counts written words and
	 * records the first written pixel so we can tell "loop ran, buffer
	 * changed" from "loop skipped / early-returned" during the freeze. */
	static int bp_total = 0;        /* cumulative band blits */
	static int bp_16 = 0;           /* cumulative 16bpp blits */
	static uint64_t bp_words = 0;   /* cumulative pixels written */
	static int bp_rows = 0;         /* rows written in the current blit */
	static uint8 bp_first[4] = {0}; /* first written byte pattern */
	static int bp_first_set = 0;

	int rows_written = 0;
	uint8 first_byte[4] = {0};
	int first_byte_set = 0;

	/* Iterate image rows in the intersection of srcRect and this band. For
	 * each image pixel (ix,iy), the destination buffer position is
	 * (iy+ty-bounds.top, ix+tx-bounds.left); clip to [0,dst_w) x [0,dst_h). */
	int iy0 = src_top, iy1 = src_bottom;
	if (iy0 < start_line) iy0 = start_line;
	if (iy1 > stop_line)  iy1 = stop_line;

	for (int iy = iy0; iy < iy1; iy++) {
		const int dy = iy + ty - (int)b_top;
		if (dy < 0 || dy >= dst_h)
			continue;
		const uint32_t *srow = frame + (size_t)iy * width;
		uint8 *drow = dst_base + (size_t)dy * row_bytes;
		for (int ix = src_left; ix < src_right; ix++) {
			const int dx = ix + tx - (int)b_left;
			if (dx < 0 || dx >= dst_w)
				continue;
			const uint32_t c = srow[ix];
			uint8 *d = drow + (size_t)dx * bytes_pp;
			if (bytes_pp == 2) {
				const uint16 v = (uint16)((((c >> 19) & 0x1F) << 10) |
				                          (((c >> 11) & 0x1F) << 5) |
				                          ((c >> 3) & 0x1F));
				d[0] = (uint8)(v >> 8);
				d[1] = (uint8)v;
			} else {
				d[0] = 0;
				d[1] = (uint8)(c >> 16);
				d[2] = (uint8)(c >> 8);
				d[3] = (uint8)c;
			}
			if (!first_byte_set) {
				first_byte[0] = d[0];
				first_byte[1] = d[1];
				first_byte[2] = (bytes_pp > 2) ? d[2] : 0;
				first_byte[3] = (bytes_pp > 2) ? d[3] : 0;
				first_byte_set = 1;
			}
			bp_words++;
		}
		rows_written++;
	}

	bp_total++;
	if (pixel_size == 16) bp_16++;
	bp_rows += rows_written;
	if (first_byte_set && !bp_first_set) {
		bp_first[0] = first_byte[0];
		bp_first[1] = first_byte[1];
		bp_first[2] = first_byte[2];
		bp_first[3] = first_byte[3];
		bp_first_set = 1;
	}

	/* Periodic probe every 8 blits (DESCENT_HITCH_DEBUG): shows whether
	 * writes happen but the compositor still reports a frozen hash. Hash over
	 * the region we actually wrote (dst_base) vs the compositor's sBufRegion
	 * (same formula) - if ours changes while the compositor's is frozen, the
	 * two pointers are different physical memory. */
#if DESCENT_HITCH_DEBUG
	static int bp_log_mod = 0;
	/* Sample ONE fixed interior movie pixel every frame (movie col 266,
	 * row 216 -> buffer offset (216+60)*row_bytes + 266*2) so we can see
	 * whether Cinepak's per-frame output actually changes during the freeze
	 * window, independent of the region hash stride. */
	const size_t sample_off = (size_t)(216 + 60) * (size_t)row_bytes + (size_t)266 * (size_t)bytes_pp;
	const uint8_t s0 = dst_base[sample_off];
	const uint8_t s1 = dst_base[sample_off + 1];
	if (++bp_log_mod >= 8) {
		bp_log_mod = 0;
		uint32_t region_hash = 2166136261u;
		const uint8_t *rb = (const uint8_t *)dst_base + (size_t)60 * row_bytes;
		for (size_t i = 0; i < (size_t)312 * row_bytes; i += 257)
			region_hash = (region_hash ^ rb[i]) * 16777619u;
		uint32_t head_hash = 2166136261u;
		for (size_t i = 0; i < 64; i += 1)
			head_hash = (head_hash ^ dst_base[i]) * 16777619u;
		CINEPAK_LOG("blitPROBE total=%d p16=%d words=%llu rows=%d "
		            "pixSize=%d firstBytes=%02x%02x%02x%02x dstHost=%p "
		            "regionHash=%08x headHash=%08x px@266,216=%02x%02x\n",
		            bp_total, bp_16, (unsigned long long)bp_words,
		            bp_rows, pixel_size,
		            bp_first[0], bp_first[1], bp_first[2], bp_first[3],
		            (void *)dst_base, region_hash, head_hash, s0, s1);
	}
#endif

	return kNoErr;
}

uint32 CinepakDispatch(uint32 componentParameters)
{
	const uint32 cp = componentParameters;
	if (cp == 0)
		return kParamErr;

	const int16 what = (int16)ReadMacInt16(cp + 2);
	const uint32 param0 = ReadMacInt32(cp + 4);

	switch (what) {
	case kComponentOpenSelect:
		CINEPAK_LOG("component OPENED (instance %08x)\n", param0);
		return kNoErr;
	case kComponentCloseSelect:
		return kNoErr;
	case kComponentCanDoSelect: {
		const int16 sel = (int16)ReadMacInt16(cp + 4);
		const bool yes = (sel >= kComponentRegisterSelect && sel <= kComponentOpenSelect) ||
		                 sel == codecGetCodecInfo ||
		                 sel == codecPreDecompress ||
		                 sel == codecBandDecompress ||
		                 sel == codecBusy;
		return yes ? 1 : 0;
	}
	case kComponentVersionSelect:
		return 0x00020001;
	case kComponentRegisterSelect:
		return kNoErr; /* 0 = stay registered */

	case codecGetCodecInfo:
		if (param0 == 0)
			return kParamErr;
		fill_codec_info(param0);
		CINEPAK_LOG("GetCodecInfo served\n");
		return kNoErr;
	case codecPreDecompress:
		return cinepak_pre_decompress(param0);
	case codecBandDecompress:
		return cinepak_band_decompress(param0);
	case codecBusy:
		return kNoErr;

	default: {
		static int logged = 0;
		if (logged < 32) {
			logged++;
			CINEPAK_LOG("unhandled selector %d (0x%04x) flags=%02x paramSize=%02x\n",
			            (int)what, (unsigned)(uint16)what,
			            (unsigned)ReadMacInt8(cp + 0),
			            (unsigned)ReadMacInt8(cp + 1));
		}
		return (what < 0) ? kBadComponentSelector : kCodecUnimpErr;
	}
	}
}

#endif /* EMULATED_PPC */
