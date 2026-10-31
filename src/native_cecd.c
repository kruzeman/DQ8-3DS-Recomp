/* CECD (StreetPass) service - faithful to the LOCAL azahar reference
   azahar-master/src/core/hle/service/cecd (cecd.cpp, cecd_u.cpp).  No external
   sources were consulted.

   The game asks SRV for "cecd:u" right after the adventure log is created; the
   whole command table of azahar's CECD_U (cecd_u.cpp:17-36) is implemented
   here in one batch so the family cannot surface one command per round.

   Backing store: azahar serves CECD out of a *system* save-data archive
   (cecd_system_save_data_archive, cecd.cpp:58) whose host tree is
   data/<sysdata>/<low>/<high>/CEC/...  This runtime keeps the same layout
   under <user-data>/cecd/CEC/... .  A console with no StreetPass history has
   an empty (or absent) CEC directory, so every read path legitimately reports
   "not found"/zero bytes - a valid EMPTY result, never zeros-as-success.

   Result codes are packed exactly like azahar core/hle/result.h
   (description 0..9, module 10..17, summary 21..26, level 27..31) with
   ErrorModule::CEC = 66, ErrorSummary::NotFound = 4, ErrorLevel::Status = 25:
     Result(NoData=1007,      CEC, NotFound, Status) = 0xC8810BEF
     Result(NotAuthorized=1002,CEC, NotFound, Status) = 0xC8810BEA
   File-system level failures inside the CEC archive reuse azahar's
   FileSys::ResultFileNotFound = 0xC8804470 / ResultPathNotFound = 0xC8804471
   (savedata_archive.cpp), exactly as native_fs.c already computes them. */
#include <sys/stat.h>
#include <dirent.h>

#define CECD_ERR_NODATA     0xC8810BEFu /* NoData (message/box file absent)   */
#define CECD_ERR_NOTAUTH    0xC8810BEAu /* NotAuthorized (dir path on file op) */
#define CECD_STATE_IDLE     1u          /* CecdState::NdmStatusIdle (cecd.h:215) */
#define CECD_ERR_PATHNF     0xC8804471u /* FileSys ResultPathNotFound          */
#define CECD_ERR_FILENF     0xC8804470u /* FileSys ResultFileNotFound          */
#define CECD_ERR_UNEXPECTED 0xE0C04702u /* FileSys UnexpectedFileOrDirectory   */

/* CecOpenMode bits (azahar cecd.h:220): 1=unknown/delete, 2=read, 4=write,
   8=create, 16=check.  CECD::Open always sets read|write|create, so an Open
   file handle is opened "r+b"; the read-only helpers (ReadMessage,
   OpenAndRead) open "rb" without create. */
#define CECD_MODE_READ   0x2u
#define CECD_MODE_WRITE  0x4u
#define CECD_MODE_CREATE 0x8u
#define CECD_MODE_CHECK  0x10u

/* The session handle SRV hands out for "cecd:u" (see main.rs service table). */
#define CECD_SERVICE_HANDLE 17u

/* CecDataPathType (azahar cecd.h:62) - directory kinds take the dir branch. */
#define CECD_PATH_ROOTDIR   10u
#define CECD_PATH_MBOXDIR   11u
#define CECD_PATH_INBOXDIR  12u
#define CECD_PATH_OUTBOXDIR 13u
#define CECD_PATH_ISDIR(t) ((t)==CECD_PATH_ROOTDIR || (t)==CECD_PATH_MBOXDIR || \
                            (t)==CECD_PATH_INBOXDIR || (t)==CECD_PATH_OUTBOXDIR)

static uint32_t cecd_program[256], cecd_path_type[256], cecd_mode[256];
static FILE *cecd_file[256];
static char cecd_path[256][2048];
static uint32_t cecd_info_event, cecd_state_event;

static void cecd_reply(uint32_t command, uint32_t normal_words, uint32_t result) {
    put32(active_tls_base + 0x80u, (command & 0xFFFF0000u) | (normal_words << 6) | 2u);
    put32(active_tls_base + 0x84u, result);
}
static int cecd_traced(void) { return getenv("DQ8_TRACE_IPC") != NULL; }

/* CryptoPP Base64Encoder with Pad=false, no line breaks (cecd.cpp:852). */
static void cecd_b64(const uint8_t *in, uint32_t n, char *out, size_t capacity) {
    static const char dict[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (uint32_t i = 0; i < n && o + 4 < capacity; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < n) v |= in[i + 2];
        out[o++] = dict[(v >> 18) & 63u];
        out[o++] = dict[(v >> 12) & 63u];
        if (i + 1 < n) out[o++] = dict[(v >> 6) & 63u];
        if (i + 2 < n) out[o++] = dict[v & 63u];
    }
    out[o] = 0;
}

/* Module::GetCecDataPathTypeAsString (cecd.cpp:872) -> host path below
   <user-data>/cecd.  Directories are created by the caller, never here. */
static uint32_t cecd_host_path(uint32_t program, uint32_t type, const uint8_t *msg_id,
                               uint32_t msg_id_size, char *out, size_t capacity) {
    char relative[1024], id64[64];
    cecd_b64(msg_id, msg_id_size, id64, sizeof(id64));
    switch (type) {
    case 1u:  snprintf(relative, sizeof(relative), "/CEC/MBoxList____"); break;
    case 2u:  snprintf(relative, sizeof(relative), "/CEC/%08X/MBoxInfo____", program); break;
    case 3u:  snprintf(relative, sizeof(relative), "/CEC/%08X/InBox___/BoxInfo_____", program); break;
    case 4u:  snprintf(relative, sizeof(relative), "/CEC/%08X/OutBox__/BoxInfo_____", program); break;
    case 5u:  snprintf(relative, sizeof(relative), "/CEC/%08X/OutBox__/OBIndex_____", program); break;
    case 6u:  snprintf(relative, sizeof(relative), "/CEC/%08X/InBox___/_%s", program, id64); break;
    case 7u:  snprintf(relative, sizeof(relative), "/CEC/%08X/OutBox__/_%s", program, id64); break;
    case 10u: snprintf(relative, sizeof(relative), "/CEC"); break;
    case 11u: snprintf(relative, sizeof(relative), "/CEC/%08X", program); break;
    case 12u: snprintf(relative, sizeof(relative), "/CEC/%08X/InBox___", program); break;
    case 13u: snprintf(relative, sizeof(relative), "/CEC/%08X/OutBox__", program); break;
    default:  /* MboxData/MboxIcon/MboxTitle and anything else >= 100 */
        snprintf(relative, sizeof(relative), "/CEC/%08X/MBoxData.%03u", program,
                 type >= 100u ? type - 100u : type);
        break;
    }
    int n = snprintf(out, capacity, "%s/cecd%s", native_fs_base(), relative);
    return (n < 0 || (size_t)n >= capacity) ? CECD_ERR_UNEXPECTED : 0u;
}

static int cecd_is_dir(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}
static int cecd_is_file(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISREG(st.st_mode);
}

/* CECD::Open, directory branch (cecd.cpp:71-101): OpenDirectory; on failure
   the create flag makes azahar CreateDirectory and report success with ZERO
   entries; on success it reads up to 32 entries and reports that count. */
static uint32_t cecd_open_dir(const char *path, uint32_t mode, uint32_t *entries) {
    *entries = 0;
    if (!cecd_is_dir(path)) {
        if (!(mode & CECD_MODE_CREATE)) return CECD_ERR_NODATA;
        if (native_fs_mkdir_p(path, 0700)) return (errno == ENOENT) ? CECD_ERR_PATHNF
                                                                   : CECD_ERR_UNEXPECTED;
        return 0;
    }
    DIR *d = opendir(path);
    if (!d) return CECD_ERR_UNEXPECTED;
    uint32_t count = 0;
    struct dirent *e;
    while ((e = readdir(d)) && count < 32u) { /* azahar max_entries = 32 */
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        ++count;
    }
    closedir(d);
    *entries = count;
    return 0;
}

/* CECD::Open, file branch (cecd.cpp:103-126): SaveDataArchive::OpenFile with
   mode read|write|create - an absent file is CREATED (savedata_archive.cpp:73)
   and reported with its size (0 for a fresh one). */
static uint32_t cecd_open_file(const char *path, uint32_t mode, FILE **out, uint64_t *size) {
    *out = NULL;
    *size = 0;
    if (cecd_is_dir(path)) return CECD_ERR_UNEXPECTED;
    if (!cecd_is_file(path)) {
        if (!(mode & CECD_MODE_CREATE)) return CECD_ERR_NODATA;
        const char *parent = strrchr(path, '/');
        if (parent) {
            char dir[2048];
            size_t len = (size_t)(parent - path);
            if (len >= sizeof(dir)) return CECD_ERR_UNEXPECTED;
            memcpy(dir, path, len);
            dir[len] = 0;
            if (native_fs_mkdir_p(dir, 0700)) return CECD_ERR_PATHNF;
        }
        FILE *created = fopen(path, "w+bx");
        if (!created) return (errno == ENOENT) ? CECD_ERR_PATHNF : CECD_ERR_UNEXPECTED;
        fclose(created);
    }
    FILE *file = fopen(path, (mode & CECD_MODE_WRITE) ? "r+b" : "rb");
    if (!file) return CECD_ERR_UNEXPECTED;
    struct stat st;
    if (!fstat(fileno(file), &st)) *size = (uint64_t)st.st_size;
    *out = file;
    return 0;
}

/* Read/write helpers over a session file (azahar DiskFile: explicit offset). */
static uint32_t cecd_file_read(FILE *file, uint32_t size, uint32_t ptr) {
    uint32_t mapped, bytes = 0;
    if (!size) return 0;
    if (size > 0x4000000u || !guest_memory_range(ptr, size, &mapped)) return 0;
    if (fseeko(file, 0, SEEK_SET)) return 0;
    clearerr(file);
    bytes = (uint32_t)fread(ram + mapped, 1, size, file);
    if (bytes < size) memset(ram + mapped + bytes, 0, size - bytes);
    return bytes;
}
static int cecd_file_write(FILE *file, uint32_t size, uint32_t ptr) {
    uint32_t mapped;
    if (!size) return 0;
    if (size > 0x4000000u || !guest_memory_range(ptr, size, &mapped)) return -1;
    if (fseeko(file, 0, SEEK_SET)) return -1;
    clearerr(file);
    if (fwrite(ram + mapped, 1, size, file) != size) return -1;
    return fflush(file) ? -1 : 0;
}

/* ReadData's CecSystemInfoType answers (cecd.cpp:624-661).  Eula and
   ParentControl are hardcoded in azahar (agreed / no parental control);
   EulaVersion comes from the CFG config block (cfg.cpp:1300), which this
   runtime has no store for, so azahar's missing-block default {minor=0,
   major=0} (u32_le data{} zero-init) is what it reports. */
static uint32_t cecd_system_info(uint32_t info_type, uint32_t size, uint32_t ptr) {
    uint32_t mapped = 0;
    uint8_t data[2] = {0, 0};
    uint32_t length = 0;
    switch (info_type) {
    case 1u: data[0] = 0; data[1] = 0; length = 2; break; /* EulaVersion {minor, major} */
    case 2u: data[0] = 1; length = 1; break;              /* Eula agreed */
    case 3u: data[0] = 0; length = 1; break;              /* no parental control */
    default: length = 0; break;                           /* azahar: empty buffer */
    }
    if (length && size && guest_memory_range(ptr, size, &mapped)) {
        uint32_t copy = length < size ? length : size;
        memcpy(ram + mapped, data, copy);
    }
    return 0;
}

static int native_cecd_dispatch(uint32_t handle, uint32_t command) {
    uint32_t tls = active_tls_base;
    uint32_t id = command >> 16;
    uint32_t session = handle;
    if (session != CECD_SERVICE_HANDLE) return 0; /* one cecd:u session, as SRV issued it */
    if (id < 1u || id > 0x12u) {
        /* 0x001E/0x001F (GetEventLog/GetEventLogStart) are NOT registered in
           azahar's cecd_u.cpp table, so an unregistered command is a real
           surface to report rather than something to answer. */
        fprintf(stderr, "unimplemented CECD command=0x%08X handle=%08X pc=%08X\n",
                command, handle, current_guest_pc);
        exit(10);
    }

    switch (id) {
    case 0x0001u: { /* Open: program_id, path_type, open_mode, PID(2) */
        uint32_t program = mem32(tls + 0x84u), type = mem32(tls + 0x88u);
        uint32_t mode = mem32(tls + 0x8Cu);
        char path[2048] = {0};
        uint32_t error = cecd_host_path(program, type, NULL, 0, path, sizeof(path));
        uint32_t entries = 0;
        uint64_t size = 0;
        if (!error && CECD_PATH_ISDIR(type)) error = cecd_open_dir(path, mode, &entries);
        else if (!error) {
            if (cecd_file[session]) { fclose(cecd_file[session]); cecd_file[session] = NULL; }
            error = cecd_open_file(path, mode, &cecd_file[session], &size);
        }
        if (!error) {
            cecd_program[session] = program;
            cecd_path_type[session] = type;
            cecd_mode[session] = mode;
            snprintf(cecd_path[session], sizeof(cecd_path[session]), "%s", path);
        }
        if (cecd_traced())
            fprintf(stderr, "CECD Open session=%08X program=%08X type=%u mode=%08X path=%s "
                            "result=%08X %s\n", session, program, type, mode, path, error,
                    CECD_PATH_ISDIR(type) ? "entries" : "size");
        cecd_reply(command, 2u, error);
        put32(tls + 0x88u, CECD_PATH_ISDIR(type) ? entries : (uint32_t)size);
        return 1;
    }
    case 0x0002u: { /* Read: write_buffer_size, then the mapped buffer */
        uint32_t size = mem32(tls + 0x84u), desc = mem32(tls + 0x88u), ptr = mem32(tls + 0x8Cu);
        uint32_t error = 0, bytes = 0;
        if (CECD_PATH_ISDIR(cecd_path_type[session]) || !cecd_file[session]) error = CECD_ERR_NOTAUTH;
        else bytes = cecd_file_read(cecd_file[session], size, ptr);
        if (cecd_file[session]) { fclose(cecd_file[session]); cecd_file[session] = NULL; }
        if (cecd_traced())
            fprintf(stderr, "CECD Read session=%08X size=%08X bytes=%08X result=%08X path=%s\n",
                    session, size, bytes, error, cecd_path[session]);
        cecd_reply(command, 2u, error);
        put32(tls + 0x88u, bytes);
        put32(tls + 0x8Cu, desc);
        put32(tls + 0x90u, ptr);
        return 1;
    }
    case 0x0003u:   /* ReadMessage */
    case 0x0004u: { /* ReadMessageWithHMAC: same read, HMAC check is log-only */
        uint32_t program = mem32(tls + 0x84u), outbox = mem32(tls + 0x88u);
        uint32_t id_size = mem32(tls + 0x8Cu), buf_size = mem32(tls + 0x90u);
        uint32_t id_desc = mem32(tls + 0x94u), id_ptr = mem32(tls + 0x98u);
        uint32_t desc2 = mem32(tls + 0x9Cu), ptr2 = mem32(tls + 0xA0u);
        uint32_t desc3 = mem32(tls + 0xA4u), ptr3 = mem32(tls + 0xA8u);
        uint8_t msg_id[64] = {0};
        uint32_t mapped = 0, error = 0, bytes = 0;
        char path[2048] = {0};
        if (id_size > sizeof(msg_id)) error = CECD_ERR_NODATA;
        else if (id_size && guest_memory_range(id_ptr, id_size, &mapped))
            for (uint32_t i = 0; i < id_size; ++i) msg_id[i] = mem8(id_ptr + i);
        if (!error)
            error = cecd_host_path(program, outbox ? 7u : 6u, msg_id, id_size, path, sizeof(path));
        FILE *message = NULL;
        uint64_t size = 0;
        if (!error) {
            error = cecd_open_file(path, 2u /* read only, no create */, &message, &size);
            if (!error && message) {
                bytes = cecd_file_read(message, buf_size, ptr2);
                fclose(message);
            }
        }
        if (cecd_traced())
            fprintf(stderr, "CECD %s session=%08X program=%08X outbox=%u id_size=%u "
                            "buf=%08X bytes=%08X result=%08X path=%s\n",
                    id == 0x0004u ? "ReadMessageWithHMAC" : "ReadMessage", session, program,
                    outbox, id_size, buf_size, bytes, error, path);
        cecd_reply(command, 2u, error);
        put32(tls + 0x88u, bytes);
        put32(tls + 0x8Cu, id_desc);
        put32(tls + 0x90u, id_ptr);
        put32(tls + 0x94u, desc2);
        put32(tls + 0x98u, ptr2);
        if (id == 0x0004u) { put32(tls + 0x9Cu, desc3); put32(tls + 0xA0u, ptr3); }
        return 1;
    }
    case 0x0005u: { /* Write: read_buffer_size + mapped buffer (session file) */
        uint32_t size = mem32(tls + 0x84u), desc = mem32(tls + 0x88u), ptr = mem32(tls + 0x8Cu);
        uint32_t error = 0;
        if (CECD_PATH_ISDIR(cecd_path_type[session]) || !cecd_file[session]) error = CECD_ERR_NOTAUTH;
        else {
            struct stat st;
            if (!fstat(fileno(cecd_file[session]), &st) && (uint64_t)st.st_size != size)
                if (ftruncate(fileno(cecd_file[session]), (off_t)size)) error = CECD_ERR_UNEXPECTED;
            if (!error && cecd_file_write(cecd_file[session], size, ptr)) error = CECD_ERR_UNEXPECTED;
        }
        if (cecd_file[session]) { fclose(cecd_file[session]); cecd_file[session] = NULL; }
        if (cecd_traced())
            fprintf(stderr, "CECD Write session=%08X size=%08X result=%08X path=%s\n",
                    session, size, error, cecd_path[session]);
        cecd_reply(command, 1u, error);
        put32(tls + 0x88u, desc);
        put32(tls + 0x8Cu, ptr);
        return 1;
    }
    case 0x0006u:   /* WriteMessage */
    case 0x0007u: { /* WriteMessageWithHMAC */
        uint32_t program = mem32(tls + 0x84u), outbox = mem32(tls + 0x88u);
        uint32_t id_size = mem32(tls + 0x8Cu), buf_size = mem32(tls + 0x90u);
        uint32_t desc = mem32(tls + 0x94u), ptr = mem32(tls + 0x98u);
        uint32_t id_desc = mem32(tls + 0x9Cu), id_ptr = mem32(tls + 0xA0u);
        uint32_t hmac_desc = mem32(tls + 0xA4u), hmac_ptr = mem32(tls + 0xA8u);
        uint8_t msg_id[64] = {0};
        uint32_t mapped = 0, error = 0;
        char path[2048] = {0};
        if (id_size > sizeof(msg_id)) error = CECD_ERR_NODATA;
        else if (id_size && guest_memory_range(id_ptr, id_size, &mapped))
            for (uint32_t i = 0; i < id_size; ++i) msg_id[i] = mem8(id_ptr + i);
        if (!error)
            error = cecd_host_path(program, outbox ? 7u : 6u, msg_id, id_size, path, sizeof(path));
        FILE *message = NULL;
        uint64_t size = 0;
        if (!error) {
            error = cecd_open_file(path, CECD_MODE_CREATE | 4u, &message, &size);
            if (!error && message) {
                if (cecd_file_write(message, buf_size, ptr)) error = CECD_ERR_UNEXPECTED;
                fclose(message);
            }
        }
        if (cecd_traced())
            fprintf(stderr, "CECD %s session=%08X program=%08X outbox=%u id_size=%u "
                            "buf=%08X result=%08X path=%s%s\n",
                    id == 0x0007u ? "WriteMessageWithHMAC" : "WriteMessage", session, program,
                    outbox, id_size, buf_size, error, path,
                    id == 0x0007u ? " (HMAC digest not recomputed: azahar's verify is log-only)"
                                  : "");
        cecd_reply(command, 1u, error);
        put32(tls + 0x88u, desc);
        put32(tls + 0x8Cu, ptr);
        put32(tls + 0x90u, id_desc);
        put32(tls + 0x94u, id_ptr);
        if (id == 0x0007u) { put32(tls + 0x98u, hmac_desc); put32(tls + 0x9Cu, hmac_ptr); }
        return 1;
    }
    case 0x0008u: { /* Delete: program_id, path_type, is_outbox, message_id_size */
        uint32_t program = mem32(tls + 0x84u), type = mem32(tls + 0x88u);
        uint32_t outbox = mem32(tls + 0x8Cu), id_size = mem32(tls + 0x90u);
        uint32_t id_desc = mem32(tls + 0x94u), id_ptr = mem32(tls + 0x98u);
        uint8_t msg_id[64] = {0};
        uint32_t mapped = 0, error = 0;
        char path[2048] = {0};
        if (id_size > sizeof(msg_id)) error = CECD_ERR_NODATA;
        else if (id_size && guest_memory_range(id_ptr, id_size, &mapped))
            for (uint32_t i = 0; i < id_size; ++i) msg_id[i] = mem8(id_ptr + i);
        if (!error) {
            uint32_t use = (id_size && !CECD_PATH_ISDIR(type)) ? (outbox ? 7u : 6u) : type;
            error = cecd_host_path(program, use, msg_id, id_size, path, sizeof(path));
        }
        if (!error && CECD_PATH_ISDIR(type)) {
            struct stat st;
            if (stat(path, &st)) error = CECD_ERR_PATHNF;
            else if (!S_ISDIR(st.st_mode)) error = CECD_ERR_UNEXPECTED;
            else { native_fs_wipe(path); rmdir(path); }
        } else if (!error) {
            if (!cecd_is_file(path)) error = CECD_ERR_FILENF;
            else if (unlink(path)) error = (errno == ENOENT) ? CECD_ERR_FILENF : CECD_ERR_UNEXPECTED;
        }
        if (cecd_traced())
            fprintf(stderr, "CECD Delete session=%08X program=%08X type=%u outbox=%u "
                            "id_size=%u result=%08X path=%s\n",
                    session, program, type, outbox, id_size, error, path);
        cecd_reply(command, 1u, error);
        put32(tls + 0x88u, id_desc);
        put32(tls + 0x8Cu, id_ptr);
        return 1;
    }
    case 0x0009u: { /* SetData: program_id, buffer_size, option + mapped buffer */
        uint32_t program = mem32(tls + 0x84u), size = mem32(tls + 0x88u);
        uint32_t option = mem32(tls + 0x8Cu), desc = mem32(tls + 0x90u), ptr = mem32(tls + 0x94u);
        uint32_t error = 0;
        if (option == 2u && size > 0) { /* update OBIndex_____ (cecd.cpp:589) */
            char path[2048] = {0};
            FILE *file = NULL;
            uint64_t fsize = 0;
            error = cecd_host_path(program, 5u, NULL, 0, path, sizeof(path));
            if (!error) error = cecd_open_file(path, CECD_MODE_CREATE | 4u, &file, &fsize);
            if (!error && file) {
                if (cecd_file_write(file, size, ptr)) error = CECD_ERR_UNEXPECTED;
                fclose(file);
            }
        }
        if (cecd_traced())
            fprintf(stderr, "CECD SetData session=%08X program=%08X size=%08X option=%u "
                            "result=%08X\n", session, program, size, option, error);
        cecd_reply(command, 1u, error);
        put32(tls + 0x88u, desc);
        put32(tls + 0x8Cu, ptr);
        return 1;
    }
    case 0x000Au: { /* ReadData: dest_size, info_type, param_size + 2 buffers */
        uint32_t dest_size = mem32(tls + 0x84u), info_type = mem32(tls + 0x88u);
        uint32_t param_size = mem32(tls + 0x8Cu);
        uint32_t pd = mem32(tls + 0x90u), pp = mem32(tls + 0x94u);
        uint32_t dd = mem32(tls + 0x98u), dp = mem32(tls + 0x9Cu);
        uint32_t error = cecd_system_info(info_type, dest_size, dp);
        if (cecd_traced())
            fprintf(stderr, "CECD ReadData session=%08X dest=%08X info_type=%u param=%08X "
                            "result=%08X\n", session, dest_size, info_type, param_size, error);
        cecd_reply(command, 1u, error);
        put32(tls + 0x88u, pd); put32(tls + 0x8Cu, pp);
        put32(tls + 0x90u, dd); put32(tls + 0x94u, dp);
        return 1;
    }
    case 0x000Bu:   /* Start: STUBBED in azahar (cecd.cpp:664) -> ResultSuccess */
    case 0x000Cu: { /* Stop:  STUBBED in azahar (cecd.cpp:674) -> ResultSuccess */
        uint32_t cec_command = mem32(tls + 0x84u);
        if (cecd_traced())
            fprintf(stderr, "CECD %s session=%08X command=%u (azahar stub: success)\n",
                    id == 0x000Bu ? "Start" : "Stop", session, cec_command);
        cecd_reply(command, 1u, 0u);
        return 1;
    }
    case 0x000Du: { /* GetCecInfoBuffer: result + echoed mapped buffer (azahar
                       pushes the buffer back untouched, cecd.cpp:684) */
        uint32_t info_type = mem32(tls + 0x84u), size = mem32(tls + 0x88u);
        uint32_t desc = mem32(tls + 0x8Cu), ptr = mem32(tls + 0x90u);
        if (cecd_traced())
            fprintf(stderr, "CECD GetCecInfoBuffer session=%08X info_type=%u size=%08X\n",
                    session, info_type, size);
        cecd_reply(command, 1u, 0u);
        put32(tls + 0x88u, desc);
        put32(tls + 0x8Cu, ptr);
        return 1;
    }
    case 0x000Eu: { /* GetCecdState -> CecdState::NdmStatusIdle (cecd.cpp:698) */
        if (cecd_traced())
            fprintf(stderr, "CECD GetCecdState session=%08X state=%u\n", session, CECD_STATE_IDLE);
        cecd_reply(command, 2u, 0u);
        put32(tls + 0x88u, CECD_STATE_IDLE);
        return 1;
    }
    case 0x000Fu:   /* GetCecInfoEventHandle  (cecd.cpp:708) */
    case 0x0010u: { /* GetChangeStateEventHandle (cecd.cpp:718) */
        uint32_t *event = id == 0x000Fu ? &cecd_info_event : &cecd_state_event;
        if (!*event) {
            if (next_event_handle >= 256u) { fputs("CECD event table exhausted\n", stderr); exit(22); }
            *event = next_event_handle++;
            event_live[*event] = 1u;
            event_manual_reset[*event] = 1u;   /* azahar's CECD events are manual-reset */
            event_signaled[*event] = 0u;       /* never signaled: no StreetPass activity */
        }
        if (cecd_traced())
            fprintf(stderr, "CECD %s session=%08X handle=%08X\n",
                    id == 0x000Fu ? "GetCecInfoEventHandle" : "GetChangeStateEventHandle",
                    session, *event);
        cecd_reply(command, 1u, 0u);
        put32(tls + 0x88u, 0x10u); /* copy-handle descriptor (1 handle) */
        put32(tls + 0x8Cu, *event);
        return 1;
    }
    case 0x0011u: { /* OpenAndWrite: buffer_size, program_id, path_type, mode, PID(2) */
        uint32_t size = mem32(tls + 0x84u), program = mem32(tls + 0x88u);
        uint32_t type = mem32(tls + 0x8Cu), mode = mem32(tls + 0x90u);
        uint32_t desc = mem32(tls + 0x9Cu), ptr = mem32(tls + 0xA0u);
        uint32_t error = 0;
        char path[2048] = {0};
        if (CECD_PATH_ISDIR(type)) error = CECD_ERR_NOTAUTH; /* azahar: NotAuthorized */
        else {
            FILE *file = NULL;
            uint64_t fsize = 0;
            error = cecd_host_path(program, type, NULL, 0, path, sizeof(path));
            if (!error) error = cecd_open_file(path, CECD_MODE_CREATE | 4u, &file, &fsize);
            if (!error && file) {
                if ((uint64_t)fsize != size && ftruncate(fileno(file), (off_t)size))
                    error = CECD_ERR_UNEXPECTED;
                if (!error && cecd_file_write(file, size, ptr)) error = CECD_ERR_UNEXPECTED;
                fclose(file);
            }
        }
        if (cecd_traced())
            fprintf(stderr, "CECD OpenAndWrite session=%08X program=%08X type=%u size=%08X "
                            "result=%08X path=%s\n", session, program, type, size, error, path);
        cecd_reply(command, 1u, error);
        put32(tls + 0x88u, desc);
        put32(tls + 0x8Cu, ptr);
        return 1;
    }
    case 0x0012u: { /* OpenAndRead: buffer_size, program_id, path_type, mode, PID(2) */
        uint32_t size = mem32(tls + 0x84u), program = mem32(tls + 0x88u);
        uint32_t type = mem32(tls + 0x8Cu), mode = mem32(tls + 0x90u);
        uint32_t desc = mem32(tls + 0x9Cu), ptr = mem32(tls + 0xA0u);
        uint32_t error = 0, bytes = 0;
        char path[2048] = {0};
        if (CECD_PATH_ISDIR(type)) error = CECD_ERR_NOTAUTH;
        else {
            FILE *file = NULL;
            uint64_t fsize = 0;
            error = cecd_host_path(program, type, NULL, 0, path, sizeof(path));
            if (!error) error = cecd_open_file(path, mode & ~CECD_MODE_CREATE, &file, &fsize);
            if (!error && file) { bytes = cecd_file_read(file, size, ptr); fclose(file); }
        }
        if (cecd_traced())
            fprintf(stderr, "CECD OpenAndRead session=%08X program=%08X type=%u size=%08X "
                            "bytes=%08X result=%08X path=%s\n", session, program, type, size,
                    bytes, error, path);
        cecd_reply(command, 2u, error);
        put32(tls + 0x88u, bytes);
        put32(tls + 0x8Cu, desc);
        put32(tls + 0x90u, ptr);
        return 1;
    }
    default:
        break;
    }
    return 0;
}
