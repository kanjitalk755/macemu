/*
 *  platform_null.cpp - Null implementations for host platform functions
 *
 *  These are the host OS functions that BasiliskII needs but we provide
 *  as no-ops for minimal testing.
 */

#include "sysdeps.h"
#include <sys/time.h>

/*
 *  Platform-specific stubs
 */

void MountVolume(const char *path)
{
	(void)path;
}

void FileDiskLayout(loff_t size, loff_t *start, loff_t *length)
{
	(void)size;
	(void)start;
	(void)length;
}

void FloppyInit()
{
}

/*
 *  System stubs
 */

void SysAddSerialPrefs()
{
}

void SysAddFloppyPrefs()
{
}

void SysAddDiskPrefs()
{
}

void SysAddCDROMPrefs()
{
}

void *Sys_open(const char *path, bool read_only, bool no_cache)
{
	(void)path;
	(void)read_only;
	(void)no_cache;
	return NULL;
}

void Sys_close(void *fh)
{
	(void)fh;
}

size_t Sys_read(void *fh, void *buf, loff_t offset, size_t length)
{
	(void)fh;
	(void)buf;
	(void)offset;
	(void)length;
	return 0;
}

size_t Sys_write(void *fh, void *buf, loff_t offset, size_t length)
{
	(void)fh;
	(void)buf;
	(void)offset;
	(void)length;
	return 0;
}

bool SysIsReadOnly(void *fh)
{
	(void)fh;
	return true;
}

bool SysIsDiskInserted(void *fh)
{
	(void)fh;
	return false;
}

bool SysIsFixedDisk(void *fh)
{
	(void)fh;
	return false;
}

loff_t SysGetFileSize(void *fh)
{
	(void)fh;
	return 0;
}

void SysEject(void *fh)
{
	(void)fh;
}

void SysAllowRemoval(void *fh)
{
	(void)fh;
}

void SysPreventRemoval(void *fh)
{
	(void)fh;
}

bool SysCDGetVolume(void *fh, uint8 &left, uint8 &right)
{
	(void)fh;
	left = 255;
	right = 255;
	return true;
}

bool SysCDSetVolume(void *fh, uint8 left, uint8 right)
{
	(void)fh;
	(void)left;
	(void)right;
	return true;
}

void SysCDPause(void *fh)
{
	(void)fh;
}

void SysCDResume(void *fh)
{
	(void)fh;
}

bool SysCDPlay(void *fh, uint8 m1, uint8 s1, uint8 f1, uint8 m2, uint8 s2, uint8 f2)
{
	(void)fh;
	(void)m1; (void)s1; (void)f1;
	(void)m2; (void)s2; (void)f2;
	return false;
}

bool SysCDStop(void *fh, uint8 m, uint8 s, uint8 f)
{
	(void)fh;
	(void)m; (void)s; (void)f;
	return true;
}

bool SysCDGetPosition(void *fh, uint8 *pos)
{
	(void)fh;
	(void)pos;
	return false;
}

bool SysCDScan(void *fh, uint8 m, uint8 s, uint8 f, bool reverse)
{
	(void)fh;
	(void)m; (void)s; (void)f;
	(void)reverse;
	return false;
}

bool SysCDReadTOC(void *fh, uint8 *toc)
{
	(void)fh;
	(void)toc;
	return false;
}

bool SysFormat(void *fh)
{
	(void)fh;
	return false;
}

/*
 *  Timer implementations (actual working implementations)
 */

void timer_current_time(struct timeval &tv)
{
	gettimeofday(&tv, NULL);
}

void timer_add_time(struct timeval &res, struct timeval a, struct timeval b)
{
	res.tv_sec = a.tv_sec + b.tv_sec;
	res.tv_usec = a.tv_usec + b.tv_usec;
	if (res.tv_usec >= 1000000) {
		res.tv_sec++;
		res.tv_usec -= 1000000;
	}
}

void timer_sub_time(struct timeval &res, struct timeval a, struct timeval b)
{
	res.tv_sec = a.tv_sec - b.tv_sec;
	res.tv_usec = a.tv_usec - b.tv_usec;
	if (res.tv_usec < 0) {
		res.tv_sec--;
		res.tv_usec += 1000000;
	}
}

int32 timer_host2mac_time(struct timeval tv)
{
	return tv.tv_sec * 1000000 + tv.tv_usec;
}

void timer_mac2host_time(struct timeval &tv, int32 mac_time)
{
	tv.tv_sec = mac_time / 1000000;
	tv.tv_usec = mac_time % 1000000;
}

int timer_cmp_time(struct timeval a, struct timeval b)
{
	if (a.tv_sec < b.tv_sec) return -1;
	if (a.tv_sec > b.tv_sec) return 1;
	if (a.tv_usec < b.tv_usec) return -1;
	if (a.tv_usec > b.tv_usec) return 1;
	return 0;
}

/*
 *  Mutex stubs (not used in minimal test)
 */

struct B2_mutex {
	int dummy;
};

B2_mutex *B2_create_mutex()
{
	return new B2_mutex();
}

void B2_delete_mutex(B2_mutex *m)
{
	delete m;
}

void B2_lock_mutex(B2_mutex *m)
{
	(void)m;
}

void B2_unlock_mutex(B2_mutex *m)
{
	(void)m;
}

/*
 *  Interrupt stubs
 */

extern uint32 InterruptFlags;

void SetInterruptFlag(uint32 flag)
{
	InterruptFlags |= flag;
}

void ClearInterruptFlag(uint32 flag)
{
	InterruptFlags &= ~flag;
}

/*
 *  CPU emulation stubs
 */

void FlushCodeCache(void *start, uint32 size)
{
	(void)start;
	(void)size;
}

/*
 *  ExtFS stubs
 */

ssize_t extfs_read(int fd, void *buf, size_t len)
{
	(void)fd; (void)buf; (void)len;
	return -1;
}

ssize_t extfs_write(int fd, void *buf, size_t len)
{
	(void)fd; (void)buf; (void)len;
	return -1;
}

int extfs_remove(const char *path)
{
	(void)path;
	return -1;
}

int extfs_rename(const char *old_path, const char *new_path)
{
	(void)old_path; (void)new_path;
	return -1;
}

void get_finfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	(void)path; (void)finfo; (void)fxinfo; (void)is_dir;
}

void set_finfo(const char *path, uint32 finfo, uint32 fxinfo, bool is_dir)
{
	(void)path; (void)finfo; (void)fxinfo; (void)is_dir;
}

void close_rfork(const char *path, int fd)
{
	(void)path; (void)fd;
}

int open_rfork(const char *path, int flag)
{
	(void)path; (void)flag;
	return -1;
}

off_t get_rfork_size(const char *path)
{
	(void)path;
	return 0;
}

const char *host_encoding_to_macroman(const char *str)
{
	return str;
}

const char *macroman_to_host_encoding(const char *str)
{
	return str;
}

void add_path_component(char *path, const char *component)
{
	(void)path; (void)component;
}

void extfs_init()
{
}

void extfs_exit()
{
}

/*
 *  Scratch memory (not used in minimal test)
 */

uint8 *ScratchMem = NULL;

/*
 *  Additional platform stubs needed for EmulOp
 */

// Video stubs
void VideoQuitFullScreen()
{
}

void VideoInterrupt()
{
}

// Timer stubs
bool tick_inhibit = false;

extern "C" uint32 TimerDateTime()
{
	return 0;  // Stub - return epoch
}

extern "C" void Microseconds(uint32 &hi, uint32 &lo)
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	uint64_t usec = (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
	hi = usec >> 32;
	lo = usec & 0xFFFFFFFF;
}

// Ethernet stub
void EtherInterrupt()
{
}

// Audio stubs
void AudioInterrupt()
{
}

bool audio_get_speaker_mute()
{
	return false;
}

void audio_set_speaker_mute(bool mute)
{
	(void)mute;
}

uint32 audio_get_speaker_volume()
{
	return 0x100;
}

void audio_set_speaker_volume(uint32 vol)
{
	(void)vol;
}

void audio_exit_stream()
{
}

bool audio_get_main_mute()
{
	return false;
}

uint32 audio_get_main_volume()
{
	return 0x100;
}

void audio_set_sample_size(int size)
{
	(void)size;
}

void audio_set_sample_rate(int rate)
{
	(void)rate;
}

void audio_set_channels(int channels)
{
	(void)channels;
}

void audio_set_main_mute(bool mute)
{
	(void)mute;
}

void audio_set_main_volume(uint32 vol)
{
	(void)vol;
}

void audio_enter_stream()
{
}

// Idle handling
void idle_wait()
{
}
