#ifndef CB_H
#define CB_H

#define CB_STRING(x) (cb_string){.value = x, .len = sizeof(x) - 1}

#define cb_rebuild_yourself(argc, argv) cb_rebuild_yourself_impl((argc), (argv), __FILE__)

#ifndef cb_cc
#if _WIN32
#if defined(__GNUC__)
#define cb_cc(cmd) cb_append(cmd, CB_STRING("cc"))
#elif defined(__clang__)
#define cb_cc(cmd) cb_append(cmd, CB_STRING("clang"))
#elif defined(_MSC_VER)
#define cb_cc(cmd) cb_append(cmd, CB_STRING("cl.exe"))
#elif defined(__TINYC__)
#define cb_cc(cmd) cb_append(cmd, CB_STRING("tcc"))
#endif
#else
#define cb_cc(cmd) cb_append(cmd, CB_STRING("cc"))
#endif
#endif // cb_cc

typedef struct {
    char *value;
    size_t len;
    size_t capacity;
} command;

typedef struct {
    char *value;
    int len;
} cb_string;

#ifdef __cplusplus
extern "C" {
#endif
void cb_append(command *cmd, cb_string str_to_append);
int cb_needs_rebuild(const char *binary_path, const char **source_paths, size_t source_paths_count);
int cb_run(command *cmd);
void cb_run_async(command *cmd);
void cb_await_all();
void cb_reset(command *cmd);
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

void cb_append(command *cmd, cb_string str_to_append) {

    /* lengths of both strings do not include the null terminator */
    cb_string prefix = CB_STRING("cmd.exe /c");

    if (!cmd->len) {
        size_t intitial_cap = str_to_append.len + 1 + prefix.len;
        cmd->value = malloc(intitial_cap);
        cmd->capacity = intitial_cap;
        cmd->len = prefix.len;
        memcpy(cmd->value, prefix.value, prefix.len);
        cmd->value[cmd->len] = '\0';
    }

    size_t required = cmd->len + str_to_append.len + 2; /* to account for space and null term */
    if (cmd->capacity < required) {
        cmd->value = realloc(cmd->value, required);
        cmd->capacity = required;
    }

    char *cb_end = &cmd->value[cmd->len];
    cb_end[0] = ' ';
    cb_end++;
    for (int i = 0; i < str_to_append.len; i++) {
        cb_end[i] = str_to_append.value[i];
    }
    cmd->len += str_to_append.len + 1; /* to account for additional space */
    cmd->value[cmd->len] = '\0';
}

int cb_run(command *cmd) {
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;

    printf("[INFO] %s\n", cmd->value);
    if (!CreateProcessA(NULL, cmd->value, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        printf("[ERROR] CreateProcess failed: %lu\n", GetLastError());
        return 0;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);

    DWORD exit_code;
    GetExitCodeProcess(pi.hProcess, &exit_code);

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    cb_reset(cmd);
    return exit_code == 0;
}

void cb_run_async(command *cmd) {
    STARTUPINFOA si = {0};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;

    printf("[INFO] %s\n", cmd->value);
    if (!CreateProcessA(NULL, cmd->value, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
    }

    cb_ctx.procs[cb_ctx.procs_count].process_handle = pi.hProcess;
    cb_ctx.procs[cb_ctx.procs_count].thread_handle = pi.hThread;
    cb_ctx.procs_count++;
    cb_reset(cmd);
}

void cb_await_all() {
    for (int i = 0; i < cb_ctx.procs_count; i++) {
        WaitForSingleObject(cb_ctx.procs[i].process_handle, INFINITE);
        /* we have to close these otherwise, they leak memory */
        CloseHandle(cb_ctx.procs[i].process_handle);
        CloseHandle(cb_ctx.procs[i].thread_handle);
    }
    cb_ctx.procs_count = 0;
}

void cb_reset(command *cmd) {
    cb_string prefix = CB_STRING("cmd.exe /c");
    memcpy(cmd->value, prefix.value, prefix.len);
    cmd->len = prefix.len;
}

void cb_rebuild_yourself_impl(int argc, char **argv, const char *source_path) {
    const char *binary_path = argv[0];

    const char *inputs[] = {source_path};
    if (cb_needs_rebuild(binary_path, inputs, 1) <= 0) {
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
    cb_append(&cmd, CB_STRING("-g"));
    cb_append(&cmd, (cb_string){.value = (char *)source_path, .len = strlen(source_path)});
    cb_append(&cmd, CB_STRING("-lkernel32 -luser32 -lshell32 -o"));
    cb_append(&cmd, (cb_string){.value = (char *)binary_path, .len = strlen(binary_path)});

    if (!cb_run(&cmd)) {
        MoveFileExA(old_binary_path, binary_path, MOVEFILE_REPLACE_EXISTING);
        ExitProcess(1);
    }

    cb_reset(&cmd);
    cb_append(&cmd, (cb_string){.value = (char *)binary_path, .len = strlen(binary_path)});
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
    FILETIME binary_path_time;
    bSuccess = GetFileTime(binary_path_fd, NULL, NULL, &binary_path_time);
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
        FILETIME source_path_time;
        bSuccess = GetFileTime(source_path_fd, NULL, NULL, &source_path_time);
        CloseHandle(source_path_fd);
        if (!bSuccess) {
            return -1;
        }

        // NOTE: if even a single input_path is fresher than output_path that's 100% rebuild
        if (CompareFileTime(&source_path_time, &binary_path_time) == 1) {
            return 1;
        }
    }

    return 0;
}

#else /* POSIX */
/* TODO: Implement POSIX */
#inlcude < sys / stat.h>

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
