/* log.c - stderr -> {original stderr, varlera.log} tee via a pipe reader
 * thread, so every existing fprintf(stderr) is mirrored without call-site
 * changes. Win32 additionally installs an unhandled-exception filter that
 * writes crash_<pid>.dmp next to the exe. */
#define _CRT_SECURE_NO_WARNINGS
#include "log.h"
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>
#include <io.h>
#include <fcntl.h>

static FILE *s_log;
static int   s_orig = -1;

static DWORD WINAPI log_reader(void *arg) {
    int rd = *(int *)arg;
    char buf[2048];
    int n;
    while ((n = _read(rd, buf, sizeof buf)) > 0) {
        if (s_orig >= 0) _write(s_orig, buf, (unsigned)n);
        if (s_log) { fwrite(buf, 1, (size_t)n, s_log); fflush(s_log); }
    }
    return 0;
}

static LONG WINAPI on_crash(EXCEPTION_POINTERS *ep) {
    char path[64];
    snprintf(path, sizeof path, "crash_%lu.dmp", GetCurrentProcessId());
    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, NULL,
                          CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {0};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        MINIDUMP_TYPE mt = (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory
                                           | MiniDumpScanMemory);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hf,
                          mt, &mei, NULL, NULL);
        CloseHandle(hf);
        if (s_log) { fputs("CRASH -> ", s_log); fputs(path, s_log); fputc('\n', s_log); fflush(s_log); }
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

void log_init(void) {
    s_log = fopen("varlera.log", "w");
    int fds[2];
    if (_pipe(fds, 8192, _O_TEXT) != 0) return;
    fflush(stderr);
    s_orig = _dup(_fileno(stderr));
    if (s_orig < 0 || _dup2(fds[1], _fileno(stderr)) != 0) { _close(fds[0]); _close(fds[1]); return; }
    _close(fds[1]);
    setvbuf(stderr, NULL, _IONBF, 0);
    static int rd;                       /* outlives caller scope */
    rd = fds[0];
    HANDLE th = CreateThread(NULL, 0, log_reader, &rd, 0, NULL);
    if (th) CloseHandle(th);
    SetUnhandledExceptionFilter(on_crash);
}

#else
#include <unistd.h>
#include <pthread.h>

static FILE *s_log;
static int   s_orig = -1;

static void *log_reader(void *arg) {
    int rd = *(int *)arg;
    char buf[2048]; ssize_t n;
    while ((n = read(rd, buf, sizeof buf)) > 0) {
        if (s_orig >= 0) { ssize_t w = write(s_orig, buf, (size_t)n); (void)w; }
        if (s_log) { fwrite(buf, 1, (size_t)n, s_log); fflush(s_log); }
    }
    return NULL;
}

void log_init(void) {
    s_log = fopen("varlera.log", "w");
    int fds[2];
    if (pipe(fds) != 0) return;
    fflush(stderr);
    s_orig = dup(STDERR_FILENO);
    if (s_orig < 0 || dup2(fds[1], STDERR_FILENO) < 0) { close(fds[0]); close(fds[1]); return; }
    close(fds[1]);
    setvbuf(stderr, NULL, _IONBF, 0);
    static int rd;
    rd = fds[0];
    pthread_t th;
    if (!pthread_create(&th, NULL, log_reader, &rd)) pthread_detach(th);
}
#endif
