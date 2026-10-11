/* log.h - varlera.log: transparent stderr tee + crash minidump. */
#ifndef LOG_H
#define LOG_H

/* Install as early as possible in main(): redirects stderr through a pipe
 * whose reader thread mirrors to both the original stderr and varlera.log
 * (truncated each run). Also installs a crash handler that writes
 * crash.dmp via MiniDumpWriteDump on Win32. */
void log_init(void);

#endif
