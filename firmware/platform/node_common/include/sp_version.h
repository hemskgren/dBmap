/**
 * Sensor Platform SemVer — authoritative source is firmware/platform/VERSION.
 * The build injects that file into SP_VERSION_STRING (see CMakeLists.txt).
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time string from platform/VERSION (e.g. "0.2.0"). */
const char *sp_version_string(void);

/** Boot/startup UART: tag `sp`, message includes version=. */
void sp_version_log_boot(void);

#ifdef __cplusplus
}
#endif
