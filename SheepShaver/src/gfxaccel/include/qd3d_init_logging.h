/*
 * qd3d_init_logging.h - Focused QuickDraw 3D/RAVE diagnostics
 *
 * ENABLE_QD3D_INIT_LOGGING is the master switch. The audio, media, graphics,
 * and timing channels are independently selected with their corresponding
 * QD3D_*_LOGGING_ENABLED definition.
 */

#ifndef QD3D_INIT_LOGGING_H
#define QD3D_INIT_LOGGING_H

#ifndef QD3D_INIT_LOGGING_ENABLED
#define QD3D_INIT_LOGGING_ENABLED 0
#endif

#ifndef QD3D_GRAPHICS_LOGGING_ENABLED
#define QD3D_GRAPHICS_LOGGING_ENABLED 0
#endif

#ifndef QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOGGING_ENABLED 0
#endif

#ifndef QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOGGING_ENABLED 0
#endif

#ifndef QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOGGING_ENABLED 0
#endif

#if QD3D_INIT_LOGGING_ENABLED

#include <cstdarg>
#include <cstdio>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace qd3d_init_logging {

static inline void log(const char *category, const char *file, int line,
                       const char *format, ...)
{
	char message[2048];
	va_list args;
	va_start(args, format);
	std::vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	message[sizeof(message) - 1] = '\0';

	char record[2304];
	std::snprintf(record, sizeof(record),
	              "[QD3D:%s] %s:%d: %s\n", category, file, line, message);
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

#if QD3D_AUDIO_LOGGING_ENABLED
#define QD3D_AUDIO_LOG(...) \
	::qd3d_init_logging::log("audio", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_AUDIO_LOG(...) do { } while (0)
#endif

#if QD3D_MEDIA_LOGGING_ENABLED
#define QD3D_MEDIA_LOG(...) \
	::qd3d_init_logging::log("media", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_MEDIA_LOG(...) do { } while (0)
#endif

#if QD3D_WAIT_LOGGING_ENABLED
#define QD3D_WAIT_LOG(...) \
	::qd3d_init_logging::log("wait", __FILE__, __LINE__, __VA_ARGS__)
#else
#define QD3D_WAIT_LOG(...) do { } while (0)
#endif

#if QD3D_GRAPHICS_LOGGING_ENABLED

#define QD3D_INIT_LOG(...) \
	::qd3d_init_logging::log("init", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_STATE_LOG(...) \
	::qd3d_init_logging::log("state", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_RESOURCE_LOG(...) \
	::qd3d_init_logging::log("resource", __FILE__, __LINE__, __VA_ARGS__)
#define QD3D_RENDER_LOG(...) \
	::qd3d_init_logging::log("render", __FILE__, __LINE__, __VA_ARGS__)

#else

#define QD3D_INIT_LOG(...) do { } while (0)
#define QD3D_STATE_LOG(...) do { } while (0)
#define QD3D_RESOURCE_LOG(...) do { } while (0)
#define QD3D_RENDER_LOG(...) do { } while (0)

#endif

#else

#define QD3D_INIT_LOG(...) do { } while (0)
#define QD3D_STATE_LOG(...) do { } while (0)
#define QD3D_RESOURCE_LOG(...) do { } while (0)
#define QD3D_RENDER_LOG(...) do { } while (0)
#define QD3D_AUDIO_LOG(...) do { } while (0)
#define QD3D_MEDIA_LOG(...) do { } while (0)
#define QD3D_WAIT_LOG(...) do { } while (0)

#endif

#endif /* QD3D_INIT_LOGGING_H */
