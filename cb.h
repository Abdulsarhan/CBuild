#ifndef CB_H
#define CB_H
#include <stdint.h>

#define CB_STR8_LIT(x) (cb_string8){.data = (uint8_t*)x, .length = sizeof(x) - 1}

#define cb_rebuild_yourself(argc, argv, ...) \
    do { \
        const char *cb__sources[] = {__FILE__, __VA_ARGS__}; \
        cb_rebuild_yourself_impl((argc), (argv), cb__sources, sizeof(cb__sources) / sizeof(cb__sources[0])); \
    } while(0)

#ifndef cb_cc
#if _WIN32
#if defined(__GNUC__)
#define cb_cc(cmd) cb_append(cmd, CB_STR8_LIT("cc"))
#elif defined(__clang__)
#define cb_cc(cmd) cb_append(cmd, CB_STR8_LIT("clang"))
#elif defined(_MSC_VER)
#define cb_cc(cmd) cb_append(cmd, CB_STR8_LIT("cl.exe"))
#elif defined(__TINYC__)
#define cb_cc(cmd) cb_append(cmd, CB_STR8_LIT("tcc"))
#endif
#else
#define cb_cc(cmd) cb_append(cmd, CB_STR8_LIT("cc"))
#endif
#endif // cb_cc

typedef uint8_t  cb_u8;
typedef uint16_t cb_u16;
typedef uint32_t cb_u32;
typedef uint64_t cb_u64;
typedef int8_t   cb_i8;
typedef int16_t  cb_i16;
typedef int32_t  cb_i32;
typedef int64_t  cb_i64;
typedef cb_i8    cb_b8;
typedef cb_i16   cb_b16;
typedef cb_i32   cb_b32;
typedef cb_i64   cb_b64;
typedef float    cb_f32;
typedef double   cb_f64;

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} command;

typedef struct {
    cb_u8 *data;
    int length;
} cb_string8;

typedef struct {
    cb_u16 *data;
    int length;
} cb_string16;

typedef struct {
    cb_u32 *data;
    int length;
} cb_string32;

#ifdef __cplusplus
extern "C" {
#endif
void cb_append(command *cmd, cb_string8 str_to_append);
int cb_needs_rebuild(const char *binary_path, const char **source_paths, size_t source_paths_count);
int cb_run(command *cmd);
void cb_run_async(command *cmd);
int cb_await_all(void);
void cb_reset(command *cmd);
int cb_copy_file(cb_string8 src, cb_string8 dst);
#ifdef __cplusplus
}
#endif

#endif /* CB_H */

#ifdef CB_IMPLEMENTATION
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

typedef struct {
    HANDLE process_handle;
    HANDLE thread_handle;
} cb_process;

typedef struct {
    cb_process procs[1024];
    int procs_count;
} cb_context;
cb_context cb_ctx;

static cb_string16 cb_utf8_to_utf_16(cb_string8 utf8_str) {
    cb_string16 out = {0};
    uint8_t *string_in = (uint8_t*)malloc(utf8_str.length + 1);
    memcpy(string_in, utf8_str.data, utf8_str.length);
    string_in[utf8_str.length] = '\0';

    int len = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)string_in, -1, NULL, 0);
    out.data = (cb_u16*)malloc(len * sizeof(cb_u16));

    int out_len = MultiByteToWideChar(CP_UTF8, 0, (LPCCH)string_in, -1, (LPWSTR)out.data, len);
    if (out_len) {
        out.length = out_len;
    } else {
        free(out.data);
        out.data = NULL;
    }

    free(string_in);
    return out;
}

void cb_append(command *cmd, cb_string8 str_to_append) {
    cb_u8 *start = str_to_append.data;
    cb_u8 *end = str_to_append.data + str_to_append.length;
    while (start < end) {
        while (start < end && *start == ' '){
            start++;
        }
        if (start >= end) {
            break;
        }
        cb_u8 *tok = start;
        while (tok < end && *tok != ' ') {
            tok++;
        }
        size_t tok_len = tok - start;

        if (cmd->count >= cmd->capacity) {
            cmd->capacity = cmd->capacity == 0 ? 8 : cmd->capacity * 2;
            cmd->items = realloc(cmd->items, cmd->capacity * sizeof(char*));
        }
        char *copy = malloc(tok_len + 1);
        memcpy(copy, start, tok_len);
        copy[tok_len] = '\0';
        cmd->items[cmd->count++] = copy;
        start = tok;
    }
}

void cb_reset(command *cmd) {
    for (size_t i = 0; i < cmd->count; i++) {
        free(cmd->items[i]);
    }
    cmd->count = 0;
}

static void cb__win32_cmd_quote(command *cmd, char **out, size_t *out_len) {
    size_t cap = 1024;
    char *buf = malloc(cap);
    size_t len = 0;

    for (size_t i = 0; i < cmd->count; i++) {
        const char *arg = cmd->items[i];
        size_t arg_len = strlen(arg);
        if (i > 0) buf[len++] = ' ';

        int needs_quote = arg_len == 0 || strpbrk(arg, " \t\n\v\"") != NULL;
        if (needs_quote) buf[len++] = '"';

        size_t backslashes = 0;
        for (size_t j = 0; j < arg_len; j++) {
            if (len + 8 >= cap) {
                cap *= 2;
                buf = realloc(buf, cap);
            }
            char x = arg[j];
            if (x == '\\') {
                backslashes++;
            } else {
                if (x == '"') {
                    for (size_t k = 0; k < 1 + backslashes; k++) buf[len++] = '\\';
                }
                backslashes = 0;
            }
            buf[len++] = x;
        }
        if (needs_quote) {
            for (size_t k = 0; k < backslashes; k++) buf[len++] = '\\';
            buf[len++] = '"';
        }
    }
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
}

static int cb__run_internal(command *cmd) {
    char *quoted = NULL;
    size_t quoted_len = 0;
    cb__win32_cmd_quote(cmd, &quoted, &quoted_len);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, quoted, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("[ERROR] CreateProcess failed: %lu\n", GetLastError());
        free(quoted);
        cb_reset(cmd);
        return -1;
    } else {
        printf("[INFO] %s\n", quoted);
    }
    free(quoted);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    cb_reset(cmd);
    return (int)exit_code;
}

int cb_run(command *cmd) {
    return cb__run_internal(cmd);
}

void cb_run_async(command *cmd) {
    char *quoted = NULL;
    size_t quoted_len = 0;
    cb__win32_cmd_quote(cmd, &quoted, &quoted_len);

    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError  = GetStdHandle(STD_ERROR_HANDLE);
    si.dwFlags |= STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, quoted, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        printf("[ERROR] CreateProcess failed: %lu\n", GetLastError());
        free(quoted);
        cb_reset(cmd);
    } else {
        printf("[INFO] %s\n", quoted);
    }
    free(quoted);
    cb_ctx.procs[cb_ctx.procs_count].process_handle = pi.hProcess;
    cb_ctx.procs[cb_ctx.procs_count].thread_handle = pi.hThread;
    cb_ctx.procs_count++;
    cb_reset(cmd);
}

int cb_await_all(void) {
    int worst = 0;
    for (int i = 0; i < cb_ctx.procs_count; i++) {
        WaitForSingleObject(cb_ctx.procs[i].process_handle, INFINITE);
        DWORD exit_code = 0;
        GetExitCodeProcess(cb_ctx.procs[i].process_handle, &exit_code);
        CloseHandle(cb_ctx.procs[i].process_handle);
        CloseHandle(cb_ctx.procs[i].thread_handle);
        if ((int)exit_code > worst) worst = (int)exit_code;
    }
    cb_ctx.procs_count = 0;
    return worst;
}

int cb_copy_file(cb_string8 src, cb_string8 dst) {
    cb_string16 src_wide = cb_utf8_to_utf_16(src);
    cb_string16 dst_wide = cb_utf8_to_utf_16(dst);
    int result = CopyFileW(src_wide.data, dst_wide.data, FALSE);
    free(src_wide.data);
    free(dst_wide.data);
    if (!result) {
        return 0;
    }
    return 1;
}

void cb_rebuild_yourself_impl(int argc, char **argv, const char **source_paths, size_t source_paths_count) {
    (void)argc;

    const char *binary_path = argv[0];

    if (cb_needs_rebuild(binary_path, source_paths, source_paths_count) <= 0) {
        return;
    }

    char old_binary_path[256];
    snprintf(old_binary_path, sizeof(old_binary_path), "%s.old", binary_path);

    if (!MoveFileExA(binary_path, old_binary_path, MOVEFILE_REPLACE_EXISTING)) {
        printf("[ERROR] Failed to rename %s: %lu\n", binary_path, GetLastError());
        ExitProcess(1);
    }

    command cmd = {0};
    cb_cc(&cmd);
    cb_append(&cmd, CB_STR8_LIT("-g"));
    cb_append(&cmd, (cb_string8){.data = (uint8_t*)source_paths[0], .length = strlen(source_paths[0])});
    cb_append(&cmd, CB_STR8_LIT("-lkernel32 -luser32 -ladvapi32 -lgdi32 -lshell32 -lcomdlg32 -lwinmm -o"));
    cb_append(&cmd, (cb_string8){.data = (uint8_t*)binary_path, .length = strlen(binary_path)});

    int result = cb__run_internal(&cmd);
    if (result != 0) {
        MoveFileExA(old_binary_path, binary_path, MOVEFILE_REPLACE_EXISTING);
        ExitProcess(1);
    }

    cb_append(&cmd, (cb_string8){.data = (uint8_t *)binary_path, .length = strlen(binary_path)});
    cb_run(&cmd);
    ExitProcess(0);
}

int cb_needs_rebuild(const char *binary_path, const char **source_paths, size_t source_paths_count) {
    BOOL bSuccess;

    HANDLE binary_path_fd = CreateFile(binary_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
    if (binary_path_fd == INVALID_HANDLE_VALUE) {
        // NOTE: if output does not exist it 100% must be rebuilt
        return -1;
    }
    FILETIME binary_path_filetime;
    bSuccess = GetFileTime(binary_path_fd, NULL, NULL, &binary_path_filetime);
    CloseHandle(binary_path_fd);
    if (!bSuccess) {
        return -1;
    }

    for (size_t i = 0; i < source_paths_count; ++i) {
        const char *source_path = source_paths[i];
        HANDLE source_path_fd = CreateFile(source_path, GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
        if (source_path_fd == INVALID_HANDLE_VALUE) {
            return -1;
        }
        FILETIME source_path_filetime;
        bSuccess = GetFileTime(source_path_fd, NULL, NULL, &source_path_filetime);
        CloseHandle(source_path_fd);
        if (!bSuccess) {
            return -1;
        }

        // NOTE: if even a single source_path is fresher than binary_path that's 100% rebuild
        uint64_t source_path_time = (uint64_t)source_path_filetime.dwHighDateTime << 32 | source_path_filetime.dwLowDateTime;
        uint64_t binary_path_time = (uint64_t)binary_path_filetime.dwHighDateTime << 32 | binary_path_filetime.dwLowDateTime;
        if (source_path_time > binary_path_time) {
            return 1;
        }
    }

    return 0;
}

#else /* POSIX */
/* TODO: Implement POSIX */
#include <sys/stat.h>

void cb_reset(command *cmd) {
    cmd->len = 0;
}

int cb_needs_rebuild(const char *output_path, const char **input_paths, size_t input_paths_count) {
    struct stat statbuf = {0};

    if (stat(output_path, &statbuf) < 0) {
        if (errno == ENOENT) {
            return 1;
        }
        return -1;
    }
    time_t output_path_time = statbuf.st_mtime;

    for (size_t i = 0; i < input_paths_count; ++i) {
        const char *input_path = input_paths[i];
        if (stat(input_path, &statbuf) < 0) {
            return -1;
        }
        time_t input_path_time = statbuf.st_mtime;
        if (input_path_time > output_path_time) {
            return 1;
        }
    }

    return 0;
}
#endif /* POSIX */
#endif /* IMPLEMENTATION */
