/*
 * qd3d_init_logging.h - Focused QuickDraw 3D/RAVE initialization diagnostics
 *
 * Enable with the CMake option ENABLE_QD3D_INIT_LOGGING=ON or by defining
 * QD3D_INIT_LOGGING_ENABLED=1 for the SheepShaver target.  This channel is
 * deliberately independent of the high-volume graphics logging controls.
 */

#ifndef QD3D_INIT_LOGGING_H
#define QD3D_INIT_LOGGING_H

#ifndef QD3D_INIT_LOGGING_ENABLED
#define QD3D_INIT_LOGGING_ENABLED 0
#endif

#if QD3D_INIT_LOGGING_ENABLED

#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace qd3d_init_logging {

static inline void log(const char *file, int line, const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';

	char record[2304];
	std::snprintf(record, sizeof(record),
	              "[QD3D:init] %s:%d: %s\n", file, line, message);
	record[sizeof(record) - 1] = '\0';

	std::fputs(record, stderr);
	std::fflush(stderr);
#if defined(_WIN32)
	// GUI builds do not necessarily have a console. This is visible in the
	// Visual Studio Output window, CDB/WinDbg, and DebugView.
	OutputDebugStringA(record);
#endif
}

} // namespace qd3d_init_logging

#define QD3D_INIT_LOG(...) \
	::qd3d_init_logging::log(__FILE__, __LINE__, __VA_ARGS__)

#else

#define QD3D_INIT_LOG(...) do { } while (0)

#endif

#endif /* QD3D_INIT_LOGGING_H */
