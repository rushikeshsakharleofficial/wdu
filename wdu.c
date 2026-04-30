#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>
#include <stdint.h>
#include <time.h>

#define WDU_VERSION L"0.3.0"

typedef enum DereferenceMode {
    DEREF_NONE = 0,
    DEREF_ARGS = 1,
    DEREF_ALL = 2
} DereferenceMode;

typedef enum TimeKind {
    TIME_NONE = 0,
    TIME_MTIME,
    TIME_ATIME,
    TIME_CTIME
} TimeKind;

typedef enum TimeStyle {
    TIME_STYLE_LONG_ISO = 0,
    TIME_STYLE_FULL_ISO,
    TIME_STYLE_ISO,
    TIME_STYLE_CUSTOM
} TimeStyle;

typedef struct PatternList {
    wchar_t **items;
    size_t len;
    size_t cap;
} PatternList;

typedef struct Options {
    int output_nul;
    int all;
    int apparent;
    int total;
    int quiet;
    int summarize;
    int human;
    int si;
    int inodes;
    int count_links;
    int separate_dirs;
    int one_file_system;
    int max_depth;
    int threshold_set;
    int threshold_negative;
    uint64_t threshold;
    uint64_t block_size;
    DereferenceMode deref;
    TimeKind time_kind;
    TimeStyle time_style;
    wchar_t time_format[128];
    wchar_t *files0_from;
    PatternList excludes;
} Options;

typedef struct FileKey {
    DWORD volume;
    DWORD index_high;
    DWORD index_low;
} FileKey;

typedef struct KeySet {
    FileKey *items;
    size_t len;
    size_t cap;
} KeySet;

typedef struct ScanResult {
    uint64_t bytes;
    uint64_t inodes;
    FILETIME newest_time;
    int had_error;
} ScanResult;

typedef struct ScanContext {
    const Options *opt;
    KeySet *seen_files;
    KeySet seen_dirs;
    DWORD root_volume;
    int root_volume_set;
} ScanContext;

static void die_oom(void) {
    fputws(L"wdu: out of memory\n", stderr);
    exit(2);
}

static wchar_t *xwcsdup(const wchar_t *s) {
    size_t n = wcslen(s) + 1;
    wchar_t *out = (wchar_t *)malloc(n * sizeof(wchar_t));
    if (!out) {
        die_oom();
    }
    memcpy(out, s, n * sizeof(wchar_t));
    return out;
}

static wchar_t *xwcsndup(const wchar_t *s, size_t n) {
    wchar_t *out = (wchar_t *)malloc((n + 1) * sizeof(wchar_t));
    if (!out) {
        die_oom();
    }
    memcpy(out, s, n * sizeof(wchar_t));
    out[n] = L'\0';
    return out;
}

static void usage(void) {
    fputws(
        L"Usage: wdu [OPTION]... [FILE]...\n"
        L"  or:  wdu [OPTION]... --files0-from=F\n"
        L"Summarize disk usage of the set of FILEs, recursively for directories.\n\n"
        L"Mandatory arguments to long options are mandatory for short options too.\n"
        L"  -0, --null            end each output line with NUL, not newline\n"
        L"  -a, --all             write counts for all files, not just directories\n"
        L"      --apparent-size   print apparent sizes, rather than disk usage\n"
        L"  -B, --block-size=SIZE scale sizes by SIZE before printing them\n"
        L"  -b, --bytes           equivalent to '--apparent-size --block-size=1'\n"
        L"  -c, --total           produce a grand total\n"
        L"  -D, --dereference-args  dereference symlinks listed on the command line\n"
        L"  -d, --max-depth=N     print directories only N or fewer levels down\n"
        L"      --files0-from=F   read NUL-terminated file names from F, or stdin if F is -\n"
        L"  -H                    equivalent to --dereference-args (-D)\n"
        L"  -h, --human-readable  print human-readable sizes (e.g., 1K 234M 2G)\n"
        L"      --inodes          list file-record counts instead of block usage\n"
        L"  -k                    like --block-size=1K\n"
        L"  -L, --dereference     dereference all symbolic links\n"
        L"  -l, --count-links     count sizes many times if hard linked\n"
        L"  -m                    like --block-size=1M\n"
        L"  -P, --no-dereference  don't follow symbolic links (default)\n"
        L"  -q, --quiet           suppress permission and traversal warnings\n"
        L"  -S, --separate-dirs   for directories do not include subdirectories\n"
        L"      --si              like -h, but use powers of 1000 not 1024\n"
        L"  -s, --summarize       display only a total for each argument\n"
        L"  -t, --threshold=SIZE  exclude entries below SIZE, or above SIZE if negative\n"
        L"      --time[=WORD]     show latest mtime, atime/access/use, or ctime/status\n"
        L"      --time-style=STYLE  full-iso, long-iso, iso, or +FORMAT\n"
        L"  -X, --exclude-from=FILE  exclude files matching patterns from FILE\n"
        L"      --exclude=PATTERN    exclude files matching PATTERN\n"
        L"  -x, --one-file-system    skip directories on different volumes\n"
        L"      --help            display this help and exit\n"
        L"      --version         output version information and exit\n\n"
        L"SIZE is an integer and optional unit: K,M,G,T,P,E,Z,Y use powers of 1024;\n"
        L"KB,MB,... use powers of 1000; KiB,MiB,... use powers of 1024.\n"
        L"Windows note: --inodes reports file-record counts, and ctime/status maps\n"
        L"to Windows creation time.\n",
        stdout);
}

static void version(void) {
    wprintf(L"wdu %ls\nWindows du-compatible disk usage utility\n", WDU_VERSION);
}

static void warn_last_error(const Options *opt, const wchar_t *path, DWORD err) {
    wchar_t *msg = NULL;
    if (opt->quiet) {
        return;
    }
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
                       FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    if (msg) {
        fwprintf(stderr, L"wdu: cannot read '%ls': %ls", path, msg);
        LocalFree(msg);
    } else {
        fwprintf(stderr, L"wdu: cannot read '%ls': error %lu\n", path, err);
    }
}

static void list_push(PatternList *list, const wchar_t *item) {
    if (list->len == list->cap) {
        size_t new_cap = list->cap ? list->cap * 2 : 8;
        wchar_t **new_items = (wchar_t **)realloc(list->items, new_cap * sizeof(wchar_t *));
        if (!new_items) {
            die_oom();
        }
        list->items = new_items;
        list->cap = new_cap;
    }
    list->items[list->len++] = xwcsdup(item);
}

static void list_free(PatternList *list) {
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static int same_key(FileKey a, FileKey b) {
    return a.volume == b.volume &&
           a.index_high == b.index_high &&
           a.index_low == b.index_low;
}

static int keyset_contains(const KeySet *set, FileKey key) {
    for (size_t i = 0; i < set->len; i++) {
        if (same_key(set->items[i], key)) {
            return 1;
        }
    }
    return 0;
}

static void keyset_add(KeySet *set, FileKey key) {
    if (keyset_contains(set, key)) {
        return;
    }
    if (set->len == set->cap) {
        size_t new_cap = set->cap ? set->cap * 2 : 256;
        FileKey *new_items = (FileKey *)realloc(set->items, new_cap * sizeof(FileKey));
        if (!new_items) {
            die_oom();
        }
        set->items = new_items;
        set->cap = new_cap;
    }
    set->items[set->len++] = key;
}

static void keyset_free(KeySet *set) {
    free(set->items);
    set->items = NULL;
    set->len = 0;
    set->cap = 0;
}

static int is_directory_separator(wchar_t ch) {
    return ch == L'\\' || ch == L'/';
}

static int is_dot_dir(const wchar_t *name) {
    return wcscmp(name, L".") == 0 || wcscmp(name, L"..") == 0;
}

static int has_wildcards(const wchar_t *path) {
    return wcspbrk(path, L"*?") != NULL;
}

static wchar_t *join_path(const wchar_t *base, const wchar_t *name) {
    size_t blen = wcslen(base);
    size_t nlen = wcslen(name);
    int need_slash = blen > 0 && !is_directory_separator(base[blen - 1]);
    wchar_t *out = (wchar_t *)malloc((blen + need_slash + nlen + 1) * sizeof(wchar_t));
    if (!out) {
        die_oom();
    }
    wcscpy(out, base);
    if (need_slash) {
        out[blen] = L'\\';
        out[blen + 1] = L'\0';
    }
    wcscat(out, name);
    return out;
}

static wchar_t *concat_path_prefix(const wchar_t *prefix, const wchar_t *name) {
    size_t plen = wcslen(prefix);
    size_t nlen = wcslen(name);
    wchar_t *out = (wchar_t *)malloc((plen + nlen + 1) * sizeof(wchar_t));
    if (!out) {
        die_oom();
    }
    wcscpy(out, prefix);
    wcscat(out, name);
    return out;
}

static wchar_t *path_prefix_for_pattern(const wchar_t *pattern) {
    const wchar_t *last_sep = NULL;
    for (const wchar_t *p = pattern; *p; p++) {
        if (is_directory_separator(*p)) {
            last_sep = p;
        }
    }
    if (!last_sep) {
        return xwcsdup(L"");
    }
    return xwcsndup(pattern, (size_t)(last_sep - pattern + 1));
}

static wchar_t *make_api_path(const wchar_t *path) {
    static const wchar_t long_prefix[] = L"\\\\?\\";
    static const wchar_t unc_prefix[] = L"\\\\?\\UNC\\";
    DWORD needed;
    wchar_t *full;
    wchar_t *out;
    size_t full_len;

    if (wcsncmp(path, long_prefix, 4) == 0) {
        return xwcsdup(path);
    }

    needed = GetFullPathNameW(path, 0, NULL, NULL);
    if (needed == 0) {
        return xwcsdup(path);
    }

    full = (wchar_t *)malloc(needed * sizeof(wchar_t));
    if (!full) {
        die_oom();
    }

    if (GetFullPathNameW(path, needed, full, NULL) == 0) {
        free(full);
        return xwcsdup(path);
    }

    full_len = wcslen(full);
    if (wcsncmp(full, L"\\\\", 2) == 0) {
        out = (wchar_t *)malloc((wcslen(unc_prefix) + full_len - 2 + 1) * sizeof(wchar_t));
        if (!out) {
            die_oom();
        }
        wcscpy(out, unc_prefix);
        wcscat(out, full + 2);
    } else {
        out = (wchar_t *)malloc((wcslen(long_prefix) + full_len + 1) * sizeof(wchar_t));
        if (!out) {
            die_oom();
        }
        wcscpy(out, long_prefix);
        wcscat(out, full);
    }

    free(full);
    return out;
}

static wchar_t *make_search_path(const wchar_t *api_path) {
    return join_path(api_path, L"*");
}

static int wildcard_match_ci(const wchar_t *pattern, const wchar_t *text) {
    while (*pattern) {
        if (*pattern == L'*') {
            while (*pattern == L'*') {
                pattern++;
            }
            if (!*pattern) {
                return 1;
            }
            while (*text) {
                if (wildcard_match_ci(pattern, text)) {
                    return 1;
                }
                text++;
            }
            return 0;
        }
        if (*pattern == L'?') {
            if (!*text) {
                return 0;
            }
            pattern++;
            text++;
            continue;
        }
        if (towlower(*pattern) != towlower(*text)) {
            return 0;
        }
        pattern++;
        text++;
    }
    return *text == L'\0';
}

static const wchar_t *base_name(const wchar_t *path) {
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p; p++) {
        if (is_directory_separator(*p)) {
            base = p + 1;
        }
    }
    return base;
}

static int pattern_has_separator(const wchar_t *pattern) {
    for (const wchar_t *p = pattern; *p; p++) {
        if (is_directory_separator(*p)) {
            return 1;
        }
    }
    return 0;
}

static int is_excluded(const Options *opt, const wchar_t *display_path) {
    const wchar_t *name = base_name(display_path);
    for (size_t i = 0; i < opt->excludes.len; i++) {
        const wchar_t *pattern = opt->excludes.items[i];
        if (pattern_has_separator(pattern)) {
            if (wildcard_match_ci(pattern, display_path)) {
                return 1;
            }
        } else {
            if (wildcard_match_ci(pattern, name)) {
                return 1;
            }
        }
    }
    return 0;
}

static uint64_t file_size_from_attrs(const WIN32_FILE_ATTRIBUTE_DATA *attrs) {
    return ((uint64_t)attrs->nFileSizeHigh << 32) | attrs->nFileSizeLow;
}

static uint64_t file_size_from_find_data(const WIN32_FIND_DATAW *fd) {
    return ((uint64_t)fd->nFileSizeHigh << 32) | fd->nFileSizeLow;
}

static FILETIME selected_time_from_attrs(const WIN32_FILE_ATTRIBUTE_DATA *attrs, const Options *opt) {
    if (opt->time_kind == TIME_ATIME) {
        return attrs->ftLastAccessTime;
    }
    if (opt->time_kind == TIME_CTIME) {
        return attrs->ftCreationTime;
    }
    return attrs->ftLastWriteTime;
}

static FILETIME selected_time_from_find_data(const WIN32_FIND_DATAW *fd, const Options *opt) {
    if (opt->time_kind == TIME_ATIME) {
        return fd->ftLastAccessTime;
    }
    if (opt->time_kind == TIME_CTIME) {
        return fd->ftCreationTime;
    }
    return fd->ftLastWriteTime;
}

static int filetime_is_zero(FILETIME ft) {
    return ft.dwLowDateTime == 0 && ft.dwHighDateTime == 0;
}

static int filetime_newer(FILETIME a, FILETIME b) {
    ULARGE_INTEGER ua;
    ULARGE_INTEGER ub;
    ua.LowPart = a.dwLowDateTime;
    ua.HighPart = a.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime;
    ub.HighPart = b.dwHighDateTime;
    return ua.QuadPart > ub.QuadPart;
}

static void update_newest(FILETIME *current, FILETIME candidate) {
    if (filetime_is_zero(*current) || filetime_newer(candidate, *current)) {
        *current = candidate;
    }
}

static uint64_t usage_size_for_file(const wchar_t *api_path, uint64_t apparent_size, const Options *opt) {
    DWORD high = 0;
    DWORD low;

    if (opt->apparent || opt->inodes) {
        return apparent_size;
    }

    low = GetCompressedFileSizeW(api_path, &high);
    if (low == INVALID_FILE_SIZE && GetLastError() != NO_ERROR) {
        return apparent_size;
    }
    return ((uint64_t)high << 32) | low;
}

static int get_path_key(const wchar_t *api_path, int is_dir, int follow_reparse,
                        FileKey *key, DWORD *links, DWORD *volume) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    HANDLE h;
    BY_HANDLE_FILE_INFORMATION info;

    if (!follow_reparse) {
        flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    }

    h = CreateFileW(api_path, FILE_READ_ATTRIBUTES,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, is_dir ? flags : (flags & ~FILE_FLAG_BACKUP_SEMANTICS), NULL);
    if (h == INVALID_HANDLE_VALUE && is_dir) {
        h = CreateFileW(api_path, FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    }
    if (h == INVALID_HANDLE_VALUE) {
        return 0;
    }

    if (!GetFileInformationByHandle(h, &info)) {
        CloseHandle(h);
        return 0;
    }

    CloseHandle(h);
    if (key) {
        key->volume = info.dwVolumeSerialNumber;
        key->index_high = info.nFileIndexHigh;
        key->index_low = info.nFileIndexLow;
    }
    if (links) {
        *links = info.nNumberOfLinks;
    }
    if (volume) {
        *volume = info.dwVolumeSerialNumber;
    }
    return 1;
}

static int should_follow_reparse(const Options *opt, int is_cmdline_arg) {
    return opt->deref == DEREF_ALL || (opt->deref == DEREF_ARGS && is_cmdline_arg);
}

static int already_counted_hardlink(ScanContext *ctx, const wchar_t *api_path, int is_dir, int follow) {
    FileKey key;
    DWORD links = 0;

    if (ctx->opt->count_links) {
        return 0;
    }

    if (!get_path_key(api_path, is_dir, follow, &key, &links, NULL)) {
        return 0;
    }

    if (links <= 1) {
        return 0;
    }

    if (keyset_contains(ctx->seen_files, key)) {
        return 1;
    }

    keyset_add(ctx->seen_files, key);
    return 0;
}

static int enter_directory(ScanContext *ctx, const wchar_t *api_path, int follow) {
    FileKey key;
    DWORD volume = 0;

    if (!get_path_key(api_path, 1, follow, &key, NULL, &volume)) {
        return 1;
    }

    if (ctx->opt->one_file_system) {
        if (!ctx->root_volume_set) {
            ctx->root_volume = volume;
            ctx->root_volume_set = 1;
        } else if (volume != ctx->root_volume) {
            return 0;
        }
    }

    if (keyset_contains(&ctx->seen_dirs, key)) {
        return 0;
    }

    keyset_add(&ctx->seen_dirs, key);
    return 1;
}

static uint64_t display_units(uint64_t value, const Options *opt) {
    if (opt->human || opt->inodes || opt->block_size == 1) {
        return value;
    }
    return (value + opt->block_size - 1) / opt->block_size;
}

static void human_size(uint64_t bytes, const Options *opt, wchar_t *buf, size_t len) {
    const wchar_t *suffixes[] = {L"B", L"K", L"M", L"G", L"T", L"P", L"E", L"Z", L"Y"};
    double value = (double)bytes;
    double base = opt->si ? 1000.0 : 1024.0;
    size_t suffix = 0;

    while (value >= base && suffix < (sizeof(suffixes) / sizeof(suffixes[0])) - 1) {
        value /= base;
        suffix++;
    }

    if (suffix == 0) {
        swprintf(buf, len, L"%lluB", (unsigned long long)bytes);
    } else if (value >= 10.0) {
        swprintf(buf, len, L"%.0f%ls", value, suffixes[suffix]);
    } else {
        swprintf(buf, len, L"%.1f%ls", value, suffixes[suffix]);
    }
}

static void format_time(FILETIME ft, const Options *opt, wchar_t *buf, size_t len) {
    ULARGE_INTEGER ull;
    time_t unix_time;
    struct tm local_tm;

    if (filetime_is_zero(ft)) {
        swprintf(buf, len, L"");
        return;
    }

    ull.LowPart = ft.dwLowDateTime;
    ull.HighPart = ft.dwHighDateTime;
    if (ull.QuadPart < 116444736000000000ULL) {
        swprintf(buf, len, L"");
        return;
    }

    unix_time = (time_t)((ull.QuadPart - 116444736000000000ULL) / 10000000ULL);
    if (localtime_s(&local_tm, &unix_time) != 0) {
        swprintf(buf, len, L"");
        return;
    }

    if (opt->time_style == TIME_STYLE_FULL_ISO) {
        wcsftime(buf, len, L"%Y-%m-%d %H:%M:%S", &local_tm);
    } else if (opt->time_style == TIME_STYLE_ISO) {
        wcsftime(buf, len, L"%Y-%m-%d", &local_tm);
    } else if (opt->time_style == TIME_STYLE_CUSTOM) {
        wcsftime(buf, len, opt->time_format, &local_tm);
    } else {
        wcsftime(buf, len, L"%Y-%m-%d %H:%M", &local_tm);
    }
}

static int passes_threshold(uint64_t metric, const Options *opt) {
    if (!opt->threshold_set) {
        return 1;
    }
    if (opt->threshold_negative) {
        return metric <= opt->threshold;
    }
    return metric >= opt->threshold;
}

static void print_entry(uint64_t bytes, uint64_t inodes, FILETIME ft,
                        const wchar_t *path, const Options *opt) {
    uint64_t metric = opt->inodes ? inodes : bytes;
    wchar_t size[64];
    wchar_t timebuf[160];

    if (!passes_threshold(metric, opt)) {
        return;
    }

    if (opt->inodes) {
        swprintf(size, sizeof(size) / sizeof(size[0]), L"%llu", (unsigned long long)metric);
    } else if (opt->human) {
        human_size(metric, opt, size, sizeof(size) / sizeof(size[0]));
    } else {
        swprintf(size, sizeof(size) / sizeof(size[0]), L"%llu",
                 (unsigned long long)display_units(metric, opt));
    }

    if (opt->time_kind != TIME_NONE) {
        format_time(ft, opt, timebuf, sizeof(timebuf) / sizeof(timebuf[0]));
        wprintf(L"%8ls\t%ls\t%ls", size, timebuf, path);
    } else if (opt->human) {
        wprintf(L"%8ls\t%ls", size, path);
    } else {
        wprintf(L"%llu\t%ls", (unsigned long long)display_units(metric, opt), path);
    }

    fputwc(opt->output_nul ? L'\0' : L'\n', stdout);
}

static int should_print_dir(int depth, const Options *opt) {
    if (opt->summarize) {
        return depth == 0;
    }
    return opt->max_depth < 0 || depth <= opt->max_depth;
}

static int should_print_file(int depth, const Options *opt) {
    if (depth == 0) {
        return 1;
    }
    if (!opt->all || opt->summarize) {
        return 0;
    }
    return opt->max_depth < 0 || depth <= opt->max_depth;
}

static ScanResult zero_result(void) {
    ScanResult result;
    memset(&result, 0, sizeof(result));
    return result;
}

static ScanResult scan_path(ScanContext *ctx, const wchar_t *api_path,
                            const wchar_t *display_path, int depth, int is_cmdline_arg);

static ScanResult scan_file(ScanContext *ctx, const wchar_t *api_path,
                            const wchar_t *display_path, int depth,
                            uint64_t apparent_size, FILETIME selected_time,
                            int is_reparse, int is_cmdline_arg) {
    ScanResult result = zero_result();
    int follow = is_reparse && should_follow_reparse(ctx->opt, is_cmdline_arg);

    if (already_counted_hardlink(ctx, api_path, 0, follow)) {
        if (should_print_file(depth, ctx->opt)) {
            print_entry(0, 0, selected_time, display_path, ctx->opt);
        }
        return result;
    }

    result.bytes = usage_size_for_file(api_path, apparent_size, ctx->opt);
    result.inodes = 1;
    result.newest_time = selected_time;

    if (should_print_file(depth, ctx->opt)) {
        print_entry(result.bytes, result.inodes, result.newest_time, display_path, ctx->opt);
    }

    return result;
}

static ScanResult scan_path(ScanContext *ctx, const wchar_t *api_path,
                            const wchar_t *display_path, int depth, int is_cmdline_arg) {
    WIN32_FILE_ATTRIBUTE_DATA attrs;
    ScanResult result = zero_result();
    int is_dir;
    int is_reparse;
    int follow;

    if (is_excluded(ctx->opt, display_path)) {
        return result;
    }

    if (!GetFileAttributesExW(api_path, GetFileExInfoStandard, &attrs)) {
        result.had_error = 1;
        warn_last_error(ctx->opt, display_path, GetLastError());
        return result;
    }

    is_dir = (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    is_reparse = (attrs.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    follow = is_reparse && should_follow_reparse(ctx->opt, is_cmdline_arg);

    if (!is_dir) {
        return scan_file(ctx, api_path, display_path, depth,
                         file_size_from_attrs(&attrs),
                         selected_time_from_attrs(&attrs, ctx->opt),
                         is_reparse, is_cmdline_arg);
    }

    result.inodes = 1;
    result.newest_time = selected_time_from_attrs(&attrs, ctx->opt);

    if (is_reparse && !follow) {
        if (should_print_dir(depth, ctx->opt)) {
            print_entry(result.bytes, result.inodes, result.newest_time, display_path, ctx->opt);
        }
        return result;
    }

    if (!enter_directory(ctx, api_path, follow)) {
        return result;
    }

    wchar_t *search = make_search_path(api_path);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(search, &fd);
    free(search);

    if (h == INVALID_HANDLE_VALUE) {
        result.had_error = 1;
        warn_last_error(ctx->opt, display_path, GetLastError());
        if (should_print_dir(depth, ctx->opt)) {
            print_entry(result.bytes, result.inodes, result.newest_time, display_path, ctx->opt);
        }
        return result;
    }

    do {
        ScanResult child;
        wchar_t *child_api;
        wchar_t *child_display;
        int child_is_dir;
        int child_is_reparse;

        if (is_dot_dir(fd.cFileName)) {
            continue;
        }

        child_api = join_path(api_path, fd.cFileName);
        child_display = join_path(display_path, fd.cFileName);

        if (is_excluded(ctx->opt, child_display)) {
            free(child_api);
            free(child_display);
            continue;
        }

        child_is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        child_is_reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

        if (child_is_dir) {
            child = scan_path(ctx, child_api, child_display, depth + 1, 0);
            if (!ctx->opt->separate_dirs) {
                result.bytes += child.bytes;
                result.inodes += child.inodes;
            }
            update_newest(&result.newest_time, child.newest_time);
            result.had_error |= child.had_error;
        } else {
            child = scan_file(ctx, child_api, child_display, depth + 1,
                              file_size_from_find_data(&fd),
                              selected_time_from_find_data(&fd, ctx->opt),
                              child_is_reparse, 0);
            result.bytes += child.bytes;
            result.inodes += child.inodes;
            update_newest(&result.newest_time, child.newest_time);
            result.had_error |= child.had_error;
        }

        free(child_api);
        free(child_display);
    } while (FindNextFileW(h, &fd));

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        result.had_error = 1;
        warn_last_error(ctx->opt, display_path, err);
    }

    FindClose(h);

    if (should_print_dir(depth, ctx->opt)) {
        print_entry(result.bytes, result.inodes, result.newest_time, display_path, ctx->opt);
    }

    return result;
}

static int parse_uint_arg(const wchar_t *s, int *out) {
    wchar_t *end = NULL;
    unsigned long value = wcstoul(s, &end, 10);
    if (!s[0] || *end != L'\0' || value > 2147483647UL) {
        return 0;
    }
    *out = (int)value;
    return 1;
}

static uint64_t unit_multiplier(const wchar_t *suffix, int *ok) {
    struct Unit {
        wchar_t ch;
        uint64_t binary;
        uint64_t decimal;
    };
    static const struct Unit units[] = {
        {L'K', 1024ULL, 1000ULL},
        {L'M', 1024ULL * 1024ULL, 1000ULL * 1000ULL},
        {L'G', 1024ULL * 1024ULL * 1024ULL, 1000ULL * 1000ULL * 1000ULL},
        {L'T', 1024ULL * 1024ULL * 1024ULL * 1024ULL, 1000ULL * 1000ULL * 1000ULL * 1000ULL},
        {L'P', 1125899906842624ULL, 1000000000000000ULL},
        {L'E', 1152921504606846976ULL, 1000000000000000000ULL},
        {L'Z', 0ULL, 0ULL},
        {L'Y', 0ULL, 0ULL}
    };
    wchar_t first;

    *ok = 1;
    if (!suffix || !*suffix) {
        return 1;
    }

    if ((suffix[0] == L'b' || suffix[0] == L'B') && suffix[1] == L'\0') {
        return 512;
    }

    first = towupper(suffix[0]);
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (first == units[i].ch) {
            if (units[i].binary == 0) {
                *ok = 0;
                return 1;
            }
            if (suffix[1] == L'\0') {
                return units[i].binary;
            }
            if ((suffix[1] == L'B' || suffix[1] == L'b') && suffix[2] == L'\0') {
                return units[i].decimal;
            }
            if ((suffix[1] == L'i' || suffix[1] == L'I') &&
                (suffix[2] == L'B' || suffix[2] == L'b') && suffix[3] == L'\0') {
                return units[i].binary;
            }
        }
    }

    *ok = 0;
    return 1;
}

static int parse_size(const wchar_t *s, int allow_sign,
                      int *negative, uint64_t *out) {
    const wchar_t *p = s;
    wchar_t *end = NULL;
    unsigned long long number;
    uint64_t mult;
    int ok = 0;

    if (negative) {
        *negative = 0;
    }

    if (allow_sign && *p == L'-') {
        if (negative) {
            *negative = 1;
        }
        p++;
    } else if (allow_sign && *p == L'+') {
        p++;
    }

    if (!*p) {
        return 0;
    }

    if (iswdigit(*p)) {
        number = wcstoull(p, &end, 10);
    } else {
        number = 1;
        end = (wchar_t *)p;
    }

    mult = unit_multiplier(end, &ok);
    if (!ok) {
        return 0;
    }

    if (mult != 0 && number > UINT64_MAX / mult) {
        return 0;
    }

    *out = (uint64_t)number * mult;
    return 1;
}

static int set_block_size(Options *opt, const wchar_t *s) {
    uint64_t size;
    if (_wcsicmp(s, L"human-readable") == 0) {
        opt->human = 1;
        opt->si = 0;
        return 1;
    }
    if (_wcsicmp(s, L"si") == 0) {
        opt->human = 1;
        opt->si = 1;
        return 1;
    }
    if (!parse_size(s, 0, NULL, &size) || size == 0) {
        return 0;
    }
    opt->human = 0;
    opt->block_size = size;
    return 1;
}

static wchar_t *utf8_or_acp_to_wide(const char *bytes, int len) {
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, len, NULL, 0);
    UINT cp = CP_UTF8;
    wchar_t *out;

    if (needed <= 0) {
        cp = CP_ACP;
        needed = MultiByteToWideChar(cp, 0, bytes, len, NULL, 0);
    }
    if (needed <= 0) {
        return xwcsdup(L"");
    }

    out = (wchar_t *)malloc((needed + 1) * sizeof(wchar_t));
    if (!out) {
        die_oom();
    }
    MultiByteToWideChar(cp, cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0, bytes, len, out, needed);
    out[needed] = L'\0';
    return out;
}

static int read_all_bytes_from_handle(HANDLE h, char **out, DWORD *out_len) {
    DWORD cap = 8192;
    DWORD len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) {
        die_oom();
    }

    for (;;) {
        DWORD got = 0;
        if (len == cap) {
            DWORD new_cap = cap * 2;
            char *new_buf = (char *)realloc(buf, new_cap);
            if (!new_buf) {
                free(buf);
                die_oom();
            }
            buf = new_buf;
            cap = new_cap;
        }
        if (!ReadFile(h, buf + len, cap - len, &got, NULL)) {
            free(buf);
            return 0;
        }
        if (got == 0) {
            break;
        }
        len += got;
    }

    *out = buf;
    *out_len = len;
    return 1;
}

static int read_files0(const Options *opt, PatternList *paths) {
    HANDLE h;
    char *bytes = NULL;
    DWORD len = 0;
    DWORD start = 0;

    if (wcscmp(opt->files0_from, L"-") == 0) {
        h = GetStdHandle(STD_INPUT_HANDLE);
        if (h == INVALID_HANDLE_VALUE || h == NULL) {
            return 0;
        }
    } else {
        wchar_t *api_path = make_api_path(opt->files0_from);
        h = CreateFileW(api_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                        FILE_ATTRIBUTE_NORMAL, NULL);
        free(api_path);
        if (h == INVALID_HANDLE_VALUE) {
            warn_last_error(opt, opt->files0_from, GetLastError());
            return 0;
        }
    }

    if (!read_all_bytes_from_handle(h, &bytes, &len)) {
        if (wcscmp(opt->files0_from, L"-") != 0) {
            CloseHandle(h);
        }
        return 0;
    }

    if (wcscmp(opt->files0_from, L"-") != 0) {
        CloseHandle(h);
    }

    for (DWORD i = 0; i <= len; i++) {
        if (i == len || bytes[i] == '\0') {
            if (i > start) {
                wchar_t *wide = utf8_or_acp_to_wide(bytes + start, (int)(i - start));
                list_push(paths, wide);
                free(wide);
            }
            start = i + 1;
        }
    }

    free(bytes);
    return 1;
}

static int read_exclude_file(Options *opt, const wchar_t *path) {
    FILE *f;
    wchar_t line[2048];

    if (_wfopen_s(&f, path, L"r, ccs=UTF-8") != 0 || !f) {
        fwprintf(stderr, L"wdu: cannot open exclude file '%ls'\n", path);
        return 0;
    }

    while (fgetws(line, sizeof(line) / sizeof(line[0]), f)) {
        size_t len = wcslen(line);
        while (len > 0 && (line[len - 1] == L'\n' || line[len - 1] == L'\r')) {
            line[--len] = L'\0';
        }
        if (len > 0) {
            list_push(&opt->excludes, line);
        }
    }

    fclose(f);
    return 1;
}

static void load_default_block_size(Options *opt) {
    wchar_t *env = NULL;
    size_t env_len = 0;
    const wchar_t *names[] = {L"DU_BLOCK_SIZE", L"BLOCK_SIZE", L"BLOCKSIZE"};

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (_wdupenv_s(&env, &env_len, names[i]) == 0 && env && *env) {
            if (set_block_size(opt, env)) {
                free(env);
                return;
            }
        }
        free(env);
        env = NULL;
    }

    if (_wdupenv_s(&env, &env_len, L"POSIXLY_CORRECT") == 0 && env && *env) {
        opt->block_size = 512;
    }
    free(env);
}

static int parse_time_word(Options *opt, const wchar_t *word) {
    opt->time_kind = TIME_MTIME;
    if (!word || !*word) {
        return 1;
    }
    if (_wcsicmp(word, L"atime") == 0 ||
        _wcsicmp(word, L"access") == 0 ||
        _wcsicmp(word, L"use") == 0) {
        opt->time_kind = TIME_ATIME;
        return 1;
    }
    if (_wcsicmp(word, L"ctime") == 0 ||
        _wcsicmp(word, L"status") == 0) {
        opt->time_kind = TIME_CTIME;
        return 1;
    }
    if (_wcsicmp(word, L"mtime") == 0 ||
        _wcsicmp(word, L"modification") == 0) {
        opt->time_kind = TIME_MTIME;
        return 1;
    }
    return 0;
}

static int parse_time_style(Options *opt, const wchar_t *style) {
    if (_wcsicmp(style, L"full-iso") == 0) {
        opt->time_style = TIME_STYLE_FULL_ISO;
        return 1;
    }
    if (_wcsicmp(style, L"long-iso") == 0) {
        opt->time_style = TIME_STYLE_LONG_ISO;
        return 1;
    }
    if (_wcsicmp(style, L"iso") == 0) {
        opt->time_style = TIME_STYLE_ISO;
        return 1;
    }
    if (style[0] == L'+') {
        opt->time_style = TIME_STYLE_CUSTOM;
        wcsncpy(opt->time_format, style + 1, sizeof(opt->time_format) / sizeof(opt->time_format[0]) - 1);
        opt->time_format[sizeof(opt->time_format) / sizeof(opt->time_format[0]) - 1] = L'\0';
        return 1;
    }
    return 0;
}

static int parse_options(int argc, wchar_t **argv, Options *opt, int *first_path) {
    int i;
    *first_path = 1;

    for (i = 1; i < argc; i++) {
        wchar_t *arg = argv[i];
        if (arg[0] != L'-' || wcscmp(arg, L"-") == 0) {
            *first_path = i;
            return 1;
        }
        if (wcscmp(arg, L"--") == 0) {
            *first_path = i + 1;
            return 1;
        }
        if (wcscmp(arg, L"--help") == 0) {
            usage();
            exit(0);
        }
        if (wcscmp(arg, L"--version") == 0) {
            version();
            exit(0);
        }
        if (wcscmp(arg, L"--null") == 0) {
            opt->output_nul = 1;
            continue;
        }
        if (wcscmp(arg, L"--all") == 0) {
            opt->all = 1;
            continue;
        }
        if (wcscmp(arg, L"--apparent-size") == 0) {
            opt->apparent = 1;
            continue;
        }
        if (wcscmp(arg, L"--bytes") == 0) {
            opt->apparent = 1;
            opt->block_size = 1;
            opt->human = 0;
            continue;
        }
        if (wcscmp(arg, L"--total") == 0) {
            opt->total = 1;
            continue;
        }
        if (wcscmp(arg, L"--dereference-args") == 0) {
            opt->deref = DEREF_ARGS;
            continue;
        }
        if (wcscmp(arg, L"--dereference") == 0) {
            opt->deref = DEREF_ALL;
            continue;
        }
        if (wcscmp(arg, L"--no-dereference") == 0) {
            opt->deref = DEREF_NONE;
            continue;
        }
        if (wcscmp(arg, L"--quiet") == 0) {
            opt->quiet = 1;
            continue;
        }
        if (wcscmp(arg, L"--human-readable") == 0) {
            opt->human = 1;
            opt->si = 0;
            continue;
        }
        if (wcscmp(arg, L"--inodes") == 0) {
            opt->inodes = 1;
            continue;
        }
        if (wcscmp(arg, L"--count-links") == 0) {
            opt->count_links = 1;
            continue;
        }
        if (wcscmp(arg, L"--separate-dirs") == 0) {
            opt->separate_dirs = 1;
            continue;
        }
        if (wcscmp(arg, L"--si") == 0) {
            opt->human = 1;
            opt->si = 1;
            continue;
        }
        if (wcscmp(arg, L"--summarize") == 0) {
            opt->summarize = 1;
            continue;
        }
        if (wcscmp(arg, L"--one-file-system") == 0) {
            opt->one_file_system = 1;
            continue;
        }
        if (wcsncmp(arg, L"--block-size=", 13) == 0) {
            if (!set_block_size(opt, arg + 13)) {
                fwprintf(stderr, L"wdu: invalid block size: %ls\n", arg + 13);
                return 0;
            }
            continue;
        }
        if (wcscmp(arg, L"--block-size") == 0) {
            if (i + 1 >= argc || !set_block_size(opt, argv[++i])) {
                fputws(L"wdu: --block-size requires a valid SIZE\n", stderr);
                return 0;
            }
            continue;
        }
        if (wcsncmp(arg, L"--max-depth=", 12) == 0) {
            if (!parse_uint_arg(arg + 12, &opt->max_depth)) {
                fwprintf(stderr, L"wdu: invalid max depth: %ls\n", arg + 12);
                return 0;
            }
            continue;
        }
        if (wcscmp(arg, L"--max-depth") == 0) {
            if (i + 1 >= argc || !parse_uint_arg(argv[++i], &opt->max_depth)) {
                fputws(L"wdu: --max-depth requires a non-negative integer\n", stderr);
                return 0;
            }
            continue;
        }
        if (wcsncmp(arg, L"--files0-from=", 14) == 0) {
            opt->files0_from = xwcsdup(arg + 14);
            continue;
        }
        if (wcscmp(arg, L"--files0-from") == 0) {
            if (i + 1 >= argc) {
                fputws(L"wdu: --files0-from requires a file name\n", stderr);
                return 0;
            }
            opt->files0_from = xwcsdup(argv[++i]);
            continue;
        }
        if (wcsncmp(arg, L"--threshold=", 12) == 0) {
            opt->threshold_set = 1;
            if (!parse_size(arg + 12, 1, &opt->threshold_negative, &opt->threshold)) {
                fwprintf(stderr, L"wdu: invalid threshold: %ls\n", arg + 12);
                return 0;
            }
            continue;
        }
        if (wcscmp(arg, L"--threshold") == 0) {
            opt->threshold_set = 1;
            if (i + 1 >= argc || !parse_size(argv[++i], 1, &opt->threshold_negative, &opt->threshold)) {
                fputws(L"wdu: --threshold requires a valid SIZE\n", stderr);
                return 0;
            }
            continue;
        }
        if (wcscmp(arg, L"--time") == 0) {
            opt->time_kind = TIME_MTIME;
            continue;
        }
        if (wcsncmp(arg, L"--time=", 7) == 0) {
            if (!parse_time_word(opt, arg + 7)) {
                fwprintf(stderr, L"wdu: invalid time word: %ls\n", arg + 7);
                return 0;
            }
            continue;
        }
        if (wcsncmp(arg, L"--time-style=", 13) == 0) {
            if (!parse_time_style(opt, arg + 13)) {
                fwprintf(stderr, L"wdu: invalid time style: %ls\n", arg + 13);
                return 0;
            }
            continue;
        }
        if (wcscmp(arg, L"--time-style") == 0) {
            if (i + 1 >= argc || !parse_time_style(opt, argv[++i])) {
                fputws(L"wdu: --time-style requires a valid STYLE\n", stderr);
                return 0;
            }
            continue;
        }
        if (wcsncmp(arg, L"--exclude=", 10) == 0) {
            list_push(&opt->excludes, arg + 10);
            continue;
        }
        if (wcscmp(arg, L"--exclude") == 0) {
            if (i + 1 >= argc) {
                fputws(L"wdu: --exclude requires a PATTERN\n", stderr);
                return 0;
            }
            list_push(&opt->excludes, argv[++i]);
            continue;
        }
        if (wcsncmp(arg, L"--exclude-from=", 15) == 0) {
            if (!read_exclude_file(opt, arg + 15)) {
                return 0;
            }
            continue;
        }
        if (wcscmp(arg, L"--exclude-from") == 0) {
            if (i + 1 >= argc || !read_exclude_file(opt, argv[++i])) {
                fputws(L"wdu: --exclude-from requires a FILE\n", stderr);
                return 0;
            }
            continue;
        }

        for (size_t j = 1; arg[j]; j++) {
            switch (arg[j]) {
            case L'0':
                opt->output_nul = 1;
                break;
            case L'a':
                opt->all = 1;
                break;
            case L'B':
                if (arg[j + 1]) {
                    if (!set_block_size(opt, arg + j + 1)) {
                        fwprintf(stderr, L"wdu: invalid block size: %ls\n", arg + j + 1);
                        return 0;
                    }
                    j = wcslen(arg) - 1;
                } else if (i + 1 >= argc || !set_block_size(opt, argv[++i])) {
                    fputws(L"wdu: -B requires a valid SIZE\n", stderr);
                    return 0;
                }
                break;
            case L'b':
                opt->apparent = 1;
                opt->block_size = 1;
                opt->human = 0;
                break;
            case L'c':
                opt->total = 1;
                break;
            case L'D':
            case L'H':
                opt->deref = DEREF_ARGS;
                break;
            case L'd':
                if (arg[j + 1]) {
                    if (!parse_uint_arg(arg + j + 1, &opt->max_depth)) {
                        fwprintf(stderr, L"wdu: invalid depth: %ls\n", arg + j + 1);
                        return 0;
                    }
                    j = wcslen(arg) - 1;
                } else if (i + 1 >= argc || !parse_uint_arg(argv[++i], &opt->max_depth)) {
                    fputws(L"wdu: -d requires a non-negative integer\n", stderr);
                    return 0;
                }
                break;
            case L'h':
                opt->human = 1;
                opt->si = 0;
                break;
            case L'k':
                opt->human = 0;
                opt->block_size = 1024;
                break;
            case L'L':
                opt->deref = DEREF_ALL;
                break;
            case L'l':
                opt->count_links = 1;
                break;
            case L'm':
                opt->human = 0;
                opt->block_size = 1024 * 1024;
                break;
            case L'P':
                opt->deref = DEREF_NONE;
                break;
            case L'q':
                opt->quiet = 1;
                break;
            case L'S':
                opt->separate_dirs = 1;
                break;
            case L's':
                opt->summarize = 1;
                break;
            case L't':
                opt->threshold_set = 1;
                if (arg[j + 1]) {
                    if (!parse_size(arg + j + 1, 1, &opt->threshold_negative, &opt->threshold)) {
                        fwprintf(stderr, L"wdu: invalid threshold: %ls\n", arg + j + 1);
                        return 0;
                    }
                    j = wcslen(arg) - 1;
                } else if (i + 1 >= argc ||
                           !parse_size(argv[++i], 1, &opt->threshold_negative, &opt->threshold)) {
                    fputws(L"wdu: -t requires a valid SIZE\n", stderr);
                    return 0;
                }
                break;
            case L'X':
                if (arg[j + 1]) {
                    if (!read_exclude_file(opt, arg + j + 1)) {
                        return 0;
                    }
                    j = wcslen(arg) - 1;
                } else if (i + 1 >= argc || !read_exclude_file(opt, argv[++i])) {
                    fputws(L"wdu: -X requires a FILE\n", stderr);
                    return 0;
                }
                break;
            case L'x':
                opt->one_file_system = 1;
                break;
            default:
                fwprintf(stderr, L"wdu: unknown option -- %lc\n", arg[j]);
                return 0;
            }
        }
    }

    *first_path = argc;
    return 1;
}

static ScanResult scan_one_argument(const wchar_t *arg, const Options *opt, KeySet *seen_files) {
    ScanContext ctx;
    ScanResult result;
    wchar_t *api_path;

    memset(&ctx, 0, sizeof(ctx));
    ctx.opt = opt;
    ctx.seen_files = seen_files;
    api_path = make_api_path(arg);
    result = scan_path(&ctx, api_path, arg, 0, 1);
    free(api_path);
    keyset_free(&ctx.seen_dirs);
    return result;
}

static ScanResult scan_pattern_argument(const wchar_t *pattern, const Options *opt, KeySet *seen_files) {
    ScanResult total = zero_result();
    wchar_t *api_pattern = make_api_path(pattern);
    wchar_t *display_prefix = path_prefix_for_pattern(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(api_pattern, &fd);
    int matched = 0;

    free(api_pattern);

    if (h == INVALID_HANDLE_VALUE) {
        free(display_prefix);
        return scan_one_argument(pattern, opt, seen_files);
    }

    do {
        ScanResult item;
        wchar_t *display_path;

        if (is_dot_dir(fd.cFileName)) {
            continue;
        }

        matched = 1;
        display_path = concat_path_prefix(display_prefix, fd.cFileName);
        item = scan_one_argument(display_path, opt, seen_files);

        total.bytes += item.bytes;
        total.inodes += item.inodes;
        update_newest(&total.newest_time, item.newest_time);
        total.had_error |= item.had_error;

        free(display_path);
    } while (FindNextFileW(h, &fd));

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_FILES) {
        total.had_error = 1;
        warn_last_error(opt, pattern, err);
    }

    FindClose(h);
    free(display_prefix);

    if (!matched) {
        return scan_one_argument(pattern, opt, seen_files);
    }

    return total;
}

static ScanResult scan_argument(const wchar_t *arg, const Options *opt, KeySet *seen_files) {
    if (has_wildcards(arg)) {
        return scan_pattern_argument(arg, opt, seen_files);
    }
    return scan_one_argument(arg, opt, seen_files);
}

static void init_options(Options *opt) {
    memset(opt, 0, sizeof(*opt));
    opt->max_depth = -1;
    opt->block_size = 1024;
    opt->deref = DEREF_NONE;
    opt->time_kind = TIME_NONE;
    opt->time_style = TIME_STYLE_LONG_ISO;
    wcscpy(opt->time_format, L"%Y-%m-%d %H:%M");
    load_default_block_size(opt);
}

int wmain(void) {
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    Options opt;
    int first_path = 1;
    int exit_code = 0;
    uint64_t grand_bytes = 0;
    uint64_t grand_inodes = 0;
    FILETIME grand_time = {0, 0};
    KeySet seen_files;
    PatternList files0_paths;

    memset(&seen_files, 0, sizeof(seen_files));
    memset(&files0_paths, 0, sizeof(files0_paths));

    if (!argv) {
        fputws(L"wdu: failed to parse command line\n", stderr);
        return 2;
    }

    init_options(&opt);

    if (!parse_options(argc, argv, &opt, &first_path)) {
        fputws(L"Try 'wdu --help' for more information.\n", stderr);
        LocalFree(argv);
        list_free(&opt.excludes);
        free(opt.files0_from);
        return 2;
    }

    if (opt.files0_from && first_path < argc) {
        fputws(L"wdu: extra operand when --files0-from is specified\n", stderr);
        LocalFree(argv);
        list_free(&opt.excludes);
        free(opt.files0_from);
        return 2;
    }

    if (opt.files0_from) {
        if (!read_files0(&opt, &files0_paths)) {
            exit_code = 1;
        }
        for (size_t i = 0; i < files0_paths.len; i++) {
            ScanResult result = scan_one_argument(files0_paths.items[i], &opt, &seen_files);
            grand_bytes += result.bytes;
            grand_inodes += result.inodes;
            update_newest(&grand_time, result.newest_time);
            if (result.had_error) {
                exit_code = 1;
            }
        }
    } else if (first_path >= argc) {
        ScanResult result = scan_one_argument(L".", &opt, &seen_files);
        grand_bytes += result.bytes;
        grand_inodes += result.inodes;
        update_newest(&grand_time, result.newest_time);
        if (result.had_error) {
            exit_code = 1;
        }
    } else {
        for (int i = first_path; i < argc; i++) {
            ScanResult result = scan_argument(argv[i], &opt, &seen_files);
            grand_bytes += result.bytes;
            grand_inodes += result.inodes;
            update_newest(&grand_time, result.newest_time);
            if (result.had_error) {
                exit_code = 1;
            }
        }
    }

    if (opt.total) {
        print_entry(grand_bytes, grand_inodes, grand_time, L"total", &opt);
    }

    keyset_free(&seen_files);
    list_free(&files0_paths);
    list_free(&opt.excludes);
    free(opt.files0_from);
    LocalFree(argv);
    return exit_code;
}
