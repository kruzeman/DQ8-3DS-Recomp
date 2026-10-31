/* Host-backed save archives. RomFS remains served by the image reader. */
#include <sys/stat.h>
#include <dirent.h>
static FILE *native_files[256];
static DIR *native_dirs[256];
static char native_archive_paths[256][1024], native_dir_paths[256][2048];
/* t_d50c9503 SAVESTATE: a guest file handle is a host FILE* and cannot cross
   a process boundary, so remember the path and open mode for every open
   handle.  native_savestate.c records {path, mode, offset} in the state file
   and reopens the same files after a load (Azahar serializes its File
   backends instead). */
static char native_file_paths[256][2048];
static uint8_t native_file_modes[256];   /* 0 = "rb", 1 = "r+b", 2 = "w+b" */
static uint32_t native_next_archive = 0x40u;
static uint32_t native_fs_error(void) {
    if (errno == ENOENT) return 0xC8804470u;
    if (errno == EEXIST) return 0xC82044B4u;
    return 0xD9004586u;
}
static void native_fs_reply(uint32_t command, uint32_t counts, uint32_t result) {
    put32(active_tls_base+0x80u,(command & 0xFFFF0000u)|counts);
    put32(active_tls_base+0x84u,result);
}
/* The save root is only a single level deep below the current working
   directory (target/userdata), so a plain mkdir() fails with ENOENT whenever
   any intermediate component is missing: running the binary from a directory
   that has no target/ subdirectory used to leave the whole save flow dead
   because the guest never obtained an archive. Create the parents too. */
static int native_fs_mkdir_p(const char *path, mode_t mode) {
    if(!path || !*path) { errno=ENOENT; return -1; }
    char buffer[1024];
    size_t length=strlen(path);
    while(length>1 && path[length-1]=='/')--length;
    if(!length || length>=sizeof(buffer)) { errno=ENAMETOOLONG; return -1; }
    memcpy(buffer,path,length);
    buffer[length]=0;
    for(char *cursor=buffer+1;*cursor;++cursor) {
        if(*cursor!='/')continue;
        *cursor=0;
        if(mkdir(buffer,mode) && errno!=EEXIST)return -1;
        struct stat st;
        if(stat(buffer,&st))return -1;
        if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR; return -1; }
        *cursor='/';
    }
    if(mkdir(buffer,mode) && errno!=EEXIST)return -1;
    struct stat st;
    if(stat(buffer,&st))return -1;
    if(!S_ISDIR(st.st_mode)) { errno=ENOTDIR; return -1; }
    return 0;
}
static const char *native_fs_base(void) {
    const char *base=getenv("DQ8_USER_DATA_DIR");
    if(!base || !*base)base="target/userdata";
    return base;
}
/* Returns 0 when the save root exists. Prints a warning only when creation
   fails (never on success) and leaves errno intact for native_fs_error(). */
static int native_fs_ensure_base(void) {
    static int warned=0;
    const char *base=native_fs_base();
    if(!native_fs_mkdir_p(base,0700))return 0;
    int failure=errno;
    if(!warned) {
        warned=1;
        fprintf(stderr,"warning: cannot create user-data directory '%s': %s\n",
                base,strerror(failure));
    }
    errno=failure;
    return -1;
}
/* Also make sure the root exists as soon as the runtime starts, not only when
   the guest first asks the FS service for an archive. */
__attribute__((constructor)) static void native_fs_init(void) {
    (void)native_fs_ensure_base();
}
static void native_fs_wipe(const char *dir) {
    DIR *d=opendir(dir);if(!d)return;
    struct dirent *e;
    while((e=readdir(d))) {
        if(!strcmp(e->d_name,".") || !strcmp(e->d_name,".."))continue;
        char path[2048];
        if(snprintf(path,sizeof(path),"%s/%s",dir,e->d_name)>=(int)sizeof(path))continue;
        struct stat st;
        if(!lstat(path,&st) && S_ISDIR(st.st_mode)) { native_fs_wipe(path);rmdir(path); }
        else unlink(path);
    }
    closedir(d);
}
static uint32_t native_fs_path(uint32_t archive, uint32_t type, uint32_t size, uint32_t ptr,
                               char *out, size_t capacity) {
    if (archive >= 256u || !native_archive_paths[archive][0]) return 0xC8804465u;
    char path[1024]; unsigned length=0; uint32_t mapped;
    if (type!=1u && (!guest_memory_range(ptr,size,&mapped) || size>2048u)) return 0xD9004676u;
    if (type==1u) path[0]=0;
    else if (type==3u || type==4u) {
        unsigned stride=type==4u?2u:1u;
        for(unsigned i=0;i+stride<=size;i+=stride) {
            uint32_t c=type==4u?mem16(ptr+i):mem8(ptr+i);
            if(!c)break;
            if(c>127u || c=='\\' || length+1>=sizeof(path))return 0xD9004676u;
            path[length++]=(char)c;
        }
        path[length]=0;
    } else return 0xD9004676u;
    const char *relative=path;while(*relative=='/')++relative;
    for(const char *part=relative;*part;) {
        const char *end=strchr(part,'/');size_t n=end?(size_t)(end-part):strlen(part);
        if(n==2 && part[0]=='.' && part[1]=='.')return 0xD9004676u;
        if(!end)break;part=end+1;
    }
    int n=snprintf(out,capacity,"%s/%s",native_archive_paths[archive],relative);
    return n<0 || (size_t)n>=capacity?0xD9004676u:0u;
}
static int native_fs_dispatch(uint32_t handle,uint32_t command) {
    uint32_t tls=active_tls_base;
    if(handle==8u && command==0x080C00C2u) {
        uint32_t kind=mem32(tls+0x84u),type=mem32(tls+0x88u),size=mem32(tls+0x8Cu);
        uint32_t path=mem32(tls+0x94u),mapped;
        if(kind!=4u && kind!=6u && kind!=7u && kind!=8u)return 0;
        uint32_t hash=0;
        if(type==2u) {
            if(size>1024u || !guest_memory_range(path,size,&mapped)) {
                native_fs_reply(command,0xC0u,0xD9004676u);return 1;
            }
            hash=2166136261u;
            for(unsigned i=0;i<size;i++)hash=(hash^mem8(path+i))*16777619u;
        }
        const char *base=native_fs_base();
        uint32_t error=0,archive=native_next_archive;
        if(archive>=256u) { native_fs_reply(command,0xC0u,0xD8E007F7u);return 1; }
        if(native_fs_ensure_base())error=native_fs_error();
        int n=snprintf(native_archive_paths[archive],1024,"%s/archive-%08X-%08X",base,kind,hash);
        if(n<0 || n>=1024)error=0xD9004676u;
        if(!error && native_fs_mkdir_p(native_archive_paths[archive],0700)) {
            int failure=errno;
            fprintf(stderr,"warning: cannot create save archive directory '%s': %s\n",
                    native_archive_paths[archive],strerror(failure));
            errno=failure;error=native_fs_error();
        }
        if(error)native_archive_paths[archive][0]=0;
        else { native_next_archive++;guest_archive_id[archive]=kind; }
        native_fs_reply(command,0xC0u,error);
        put32(tls+0x88u,error?0:archive);put32(tls+0x8Cu,0u);
        return 1;
    }
    if(handle==8u && command==0x084C0242u) {
        /* FS:FormatSaveData. Issued right after the "New save data will be
           created." prompt: the guest formats the SaveData archive before
           writing a fresh save (azahar fs_user.cpp: only ArchiveIdCode::
           SaveData=4 is valid; the response is a single Result word). In this
           host-backed runtime the archive is a directory under the user-data
           dir, so formatting = ensure the directory exists and wipe it. The
           directory name mirrors OpenArchive's so the subsequently opened
           archive maps to the same host directory. */
        uint32_t kind=mem32(tls+0x84u),type=mem32(tls+0x88u),size=mem32(tls+0x8Cu);
        uint32_t path=mem32(tls+0xACu),mapped;
        if(kind!=4u)return 0;
        uint32_t hash=0;
        if(type==2u) {
            if(size>1024u || !guest_memory_range(path,size,&mapped)) {
                native_fs_reply(command,0x40u,0xD9004676u);return 1;
            }
            hash=2166136261u;
            for(unsigned i=0;i<size;i++)hash=(hash^mem8(path+i))*16777619u;
        }
        const char *base=native_fs_base();
        char dir[1024];uint32_t error=0;
        int n=snprintf(dir,sizeof(dir),"%s/archive-%08X-%08X",base,kind,hash);
        if(n<0 || (size_t)n>=sizeof(dir))error=0xD9004676u;
        if(!error && native_fs_ensure_base())error=native_fs_error();
        if(!error && native_fs_mkdir_p(dir,0700)) {
            int failure=errno;
            fprintf(stderr,"warning: cannot create save archive directory '%s': %s\n",
                    dir,strerror(failure));
            errno=failure;error=native_fs_error();
        }
        if(!error)native_fs_wipe(dir);
        if(getenv("DQ8_TRACE_IPC"))
            fprintf(stderr,"FS FormatSaveData archive=%08X dir=%s result=%08X\n",kind,dir,error);
        native_fs_reply(command,0x40u,error);
        return 1;
    }
    if(handle==8u && (command>>16)==0x080Du) {
        /* FS:ControlArchive (azahar fs_user.cpp:1809). Reached while the guest
           writes the fresh save right after the "New save data will be
           created." prompt. Layout (fs_user.h ControlArchive): 1-2 archive
           handle, 3 action, 4 input size, 5 output size, 6 (in<<4)|0xA,
           7 input pointer, 8 (out<<4)|0xC, 9 output pointer. azahar's default
           ArchiveBackend::Control is a STUB that only logs and returns
           ResultSuccess (only the network Artic archive overrides it), and it
           pushes a zero-initialised output buffer back to the guest, so mirror
           that: reply success and zero-fill the output buffer. Any handle the
           guest legitimately holds was allocated by this runtime (< 256); a
           wider handle is answered with ResultInvalidArchiveHandle like
           azahar's failed handle_map lookup. */
        uint32_t arch=mem32(tls+0x84u),hi=mem32(tls+0x88u);
        uint32_t out_size=mem32(tls+0x94u),out_ptr=mem32(tls+0xA4u),mapped;
        uint32_t error=0;
        if(hi || arch>=256u)error=0xC8804465u;
        else if(out_size) {
            if(out_size>0x4000000u || !guest_memory_range(out_ptr,out_size,&mapped))error=0xD9004676u;
            else memset(ram+mapped,0,out_size);
        }
        if(getenv("DQ8_TRACE_IPC"))
            fprintf(stderr,"FS ControlArchive archive=%08X action=%08X in=%08X out=%08X result=%08X\n",
                    arch,mem32(tls+0x8Cu),mem32(tls+0x90u),out_size,error);
        native_fs_reply(command,0x40u,error);
        return 1;
    }
    if(handle==8u && (command>>16)==0x0804u) {
        /* FS:DeleteFile (azahar fs_user.cpp:250). TLS layout: +0x84
           TransactionId, +0x88/+0x8C ArchiveHandle (u64), +0x90 LowPathType
           (4 = UTF-16), +0x94 path byte size, +0x98 static-buffer descriptor,
           +0x9C path pointer. SaveDataArchive::DeleteFile (azahar
           savedata_archive.cpp:97) checks the host status BEFORE deleting:
           missing file (NotFound) or a directory (DirectoryFound) both return
           ResultFileNotFound = FileNotFound(112)/FS(17)/NotFound(4)/Status(25)
           = 0xC8804470 - the same code native_fs_error() already maps ENOENT
           to - and only an existing regular file is unlinked (FileUtil::Delete
           -> ResultSuccess). Observed in the post-create save flow: command
           0x08040142 pc=0x003013A4 archive handle 0x47. */
        uint32_t archive=mem32(tls+0x88u),high=mem32(tls+0x8Cu);
        uint32_t type=mem32(tls+0x90u),size=mem32(tls+0x94u),ptr=mem32(tls+0x9Cu);
        char path[2048]={0};
        uint32_t error=high?0xC8804465u:native_fs_path(archive,type,size,ptr,path,sizeof(path));
        if(!error) {
            struct stat st;
            if(!stat(path,&st) && S_ISDIR(st.st_mode)) error=0xC8804470u;
            else if(unlink(path)) error=(errno==ENOENT)?0xC8804470u:native_fs_error();
        }
        if(getenv("DQ8_TRACE_IPC"))
            fprintf(stderr,"FS DeleteFile archive=%08X path=%s result=%08X\n",archive,path,error);
        native_fs_reply(command,0x40u,error);
        return 1;
    }
    if(handle==8u && (command>>16)==0x0805u) {
        /* FS:RenameFile (azahar fs_user.cpp:294). Layout: +0x84 TransactionId,
           +0x88/+0x8C src ArchiveHandle (u64), +0x90 src LowPathType, +0x94
           src size, +0x98/+0x9C dest ArchiveHandle (u64), +0xA0 dest type,
           +0xA4 dest size, then src descriptor/pointer (+0xA8/+0xAC) and dest
           descriptor/pointer (+0xB0/+0xB4). SaveDataArchive::RenameFile
           validates both paths and then FileUtil::Rename() (POSIX rename:
           atomic and replaces an existing destination); any failure returns
           the generic Result(NoData=1007, FS, NothingHappened=1, Status=25)
           = 0xC82047EF, NOT a NotFound code (azahar savedata_archive.cpp:131). */
        uint32_t sarch=mem32(tls+0x88u),shigh=mem32(tls+0x8Cu);
        uint32_t stype=mem32(tls+0x90u),ssize=mem32(tls+0x94u);
        uint32_t darch=mem32(tls+0x98u),dhigh=mem32(tls+0x9Cu);
        uint32_t dtype=mem32(tls+0xA0u),dsize=mem32(tls+0xA4u);
        uint32_t sptr=mem32(tls+0xACu),dptr=mem32(tls+0xB4u);
        char src[2048]={0},dst[2048]={0};
        uint32_t error=0;
        if(shigh||dhigh)error=0xC8804465u;
        if(!error)error=native_fs_path(sarch,stype,ssize,sptr,src,sizeof(src));
        if(!error)error=native_fs_path(darch,dtype,dsize,dptr,dst,sizeof(dst));
        if(!error && rename(src,dst))error=0xC82047EFu;
        if(getenv("DQ8_TRACE_IPC"))
            fprintf(stderr,"FS RenameFile %s -> %s result=%08X\n",src,dst,error);
        native_fs_reply(command,0x40u,error);
        return 1;
    }
    if(handle==8u && ((command>>16)==0x0806u || (command>>16)==0x0807u)) {
        /* FS:DeleteDirectory (0x0806) / DeleteDirectoryRecursively (0x0807)
           (azahar fs_user.cpp:354/399). Same layout as DeleteFile: +0x84
           TransactionId, +0x88/+0x8C ArchiveHandle, +0x90 type, +0x94 size,
           +0x98 descriptor, +0x9C path pointer. Both go through azahar's
           DeleteDirectoryHelper (savedata_archive.cpp:160): root directory =>
           ResultDirectoryNotEmpty (0xC82044F0), missing => ResultPathNotFound
           (0xC8804471), a file where a directory was asked =>
           ResultUnexpectedFileOrDirectory (0xE0C04702); then rmdir (empty
           only, ENOTEMPTY => DirectoryNotEmpty) resp. wipe+rmdir. Part of the
           same save-flow FS stub cluster as DeleteFile (wrappers at
           0x003017C0/0x00301968 next to 0x003013B8). */
        uint32_t recursive=(command>>16)==0x0807u;
        uint32_t archive=mem32(tls+0x88u),high=mem32(tls+0x8Cu);
        uint32_t type=mem32(tls+0x90u),size=mem32(tls+0x94u),ptr=mem32(tls+0x9Cu);
        char path[2048]={0};
        uint32_t error=high?0xC8804465u:native_fs_path(archive,type,size,ptr,path,sizeof(path));
        if(!error && type==1u)error=0xC82044F0u; /* azahar: root dir is never deleted */
        if(!error) {
            struct stat st;
            if(stat(path,&st))error=(errno==ENOENT)?0xC8804471u:native_fs_error();
            else if(!S_ISDIR(st.st_mode))error=0xE0C04702u;
            else if(recursive) { native_fs_wipe(path); if(rmdir(path) && errno!=ENOENT)error=native_fs_error(); }
            else if(rmdir(path))error=(errno==ENOTEMPTY)?0xC82044F0u:native_fs_error();
        }
        if(getenv("DQ8_TRACE_IPC"))
            fprintf(stderr,"FS %s archive=%08X path=%s result=%08X\n",
                    recursive?"DeleteDirRecursive":"DeleteDir",archive,path,error);
        native_fs_reply(command,0x40u,error);
        return 1;
    }
    if(handle==8u && (command==0x080201C2u || command==0x080801C2u || (command>>16)==0x0809u)) {
        uint32_t archive=mem32(tls+0x88u),high=mem32(tls+0x8Cu);
        if(archive>=256u || !native_archive_paths[archive][0])return 0;
        char path[2048]={0};uint32_t error=high?0xC8804465u:native_fs_path(archive,mem32(tls+0x90u),mem32(tls+0x94u),
            mem32(tls+(command==0x080801C2u?0xA8u:command==0x080201C2u?0xA4u:0xA0u)),path,sizeof(path));
        FILE *file=NULL;uint32_t session=0;
        if(!error && command==0x080201C2u) {
            uint32_t flags=mem32(tls+0x98u);
            int mode=(flags&2u)?1:0;
            file=fopen(path,(flags&2u)?"r+b":"rb");
            if(!file && errno==ENOENT && (flags&4u)) { file=fopen(path,"w+b"); mode=2; }
            if(!file)error=native_fs_error();
            else {
                session=guest_alloc_ipc_handle();native_files[session]=file;guest_file_is_romfs[session]=0;
                snprintf(native_file_paths[session],sizeof native_file_paths[session],"%s",path);
                native_file_modes[session]=(uint8_t)mode;
            }
        } else if(!error && command==0x080801C2u) {
            uint64_t length=(uint64_t)mem32(tls+0x9Cu)|((uint64_t)mem32(tls+0xA0u)<<32);
            if(length>0x4000000u)error=0xD9004676u;
            else {
                file=fopen(path,"w+bx");
                if(!file)error=native_fs_error();
                else { if(ftruncate(fileno(file),(off_t)length))error=native_fs_error();fclose(file); }
            }
        } else if(!error && native_fs_mkdir_p(path,0700))error=native_fs_error();
        if(getenv("DQ8_TRACE_IPC"))fprintf(stderr,"native FS path=%s command=%08X result=%08X handle=%08X\n",error&&high?"<invalid>":path,command,error,session);
        native_fs_reply(command,command==0x080201C2u && !error?0x42u:0x40u,error);
        if(session) { put32(tls+0x88u,0x10u);put32(tls+0x8Cu,session); }
        return 1;
    }
    if(handle==8u && command==0x080B0102u) {
        uint32_t archive=mem32(tls+0x84u);
        if(archive>=256u || !native_archive_paths[archive][0])return 0;
        char path[2048]={0};uint32_t error=mem32(tls+0x88u)?0xC8804465u:native_fs_path(archive,mem32(tls+0x8Cu),mem32(tls+0x90u),mem32(tls+0x98u),path,sizeof(path));
        DIR *dir=NULL;uint32_t session=0;
        if(!error && !(dir=opendir(path)))error=native_fs_error();
        if(!error) { session=guest_alloc_ipc_handle();native_dirs[session]=dir;guest_dir_open[session]=1;snprintf(native_dir_paths[session],2048,"%s",path); }
        native_fs_reply(command,error?0x40u:0x42u,error);
        if(session) { put32(tls+0x88u,0x10u);put32(tls+0x8Cu,session); }
        return 1;
    }
    if(handle<256u && native_files[handle]) {
        FILE *file=native_files[handle];uint32_t error=0;
        if(command==0x080200C2u || command==0x08030102u) {
            int write=command==0x08030102u;
            uint64_t offset=(uint64_t)mem32(tls+0x84u)|((uint64_t)mem32(tls+0x88u)<<32);
            uint32_t size=mem32(tls+0x8Cu),desc=mem32(tls+(write?0x94u:0x90u)),ptr=mem32(tls+(write?0x98u:0x94u)),mapped,bytes=0;
            if(offset>INT64_MAX || size>0x4000000u || !guest_memory_range(ptr,size,&mapped))error=0xD9004676u;
            else if(fseeko(file,(off_t)offset,SEEK_SET))error=native_fs_error();
            else {
                clearerr(file);
                bytes=write?(uint32_t)fwrite(ram+mapped,1,size,file):(uint32_t)fread(ram+mapped,1,size,file);
                if(ferror(file)) { error=native_fs_error(); if(write)bytes=0; }
                /* A09: fwrite() can accept every byte into the stdio buffer and only
                   reach the host at flush time. The old code called fflush() here but
                   ignored its result, so an ENOSPC / I-O failure on flush was reported
                   to the guest as a fully successful save write (false success). Now the
                   flush result is checked and surfaced; on failure bytes is reported as 0
                   like azahar's File::Write, which pushes 0 written whenever backend->Write
                   fails (fs/file.cpp:209-211). A short fwrite (ferror with bytes<size) is
                   likewise a failed write and reports 0. NOTE durability: this only makes
                   the stdio flush honest; it does NOT add fsync-level durability — data the
                   OS already accepted can still be lost to a later power cut. True
                   power-loss durability is a separate policy decision, not promised here. */
                else if(write && fflush(file)) { error=native_fs_error(); bytes=0; }
            }
            native_fs_reply(command,0x82u,error);put32(tls+0x88u,bytes);put32(tls+0x8Cu,desc);put32(tls+0x90u,ptr);return 1;
        }
        if(command==0x08040000u) {
            struct stat st;uint64_t size=0;if(fstat(fileno(file),&st))error=native_fs_error();else size=(uint64_t)st.st_size;
            native_fs_reply(command,0xC0u,error);put32(tls+0x88u,(uint32_t)size);put32(tls+0x8Cu,(uint32_t)(size>>32));return 1;
        }
        if(command==0x080C0000u) {
            /* FSFILE:OpenLinkFile (azahar file.cpp:385 — a stub that returns a
               new client session to the same file backend with offset reset).
               Alias the same FILE* under a fresh synthetic handle. Reads/writes
               fseeko() explicitly before every I/O so the shared position is
               safe; Close() below only fcloses the FILE* once no handle still
               references it, so the original and the link can be closed in any
               order without a double-free. */
            uint32_t link=guest_alloc_ipc_handle();
            native_files[link]=file;
            if(link<sizeof(guest_file_is_romfs))guest_file_is_romfs[link]=guest_file_is_romfs[handle];
            snprintf(native_file_paths[link],sizeof native_file_paths[link],"%s",native_file_paths[handle]);
            native_file_modes[link]=native_file_modes[handle];
            if(getenv("DQ8_TRACE_IPC"))
                fprintf(stderr,"FS OpenLinkFile file=%08X -> link=%08X\n",handle,link);
            native_fs_reply(command,0x42u,0);
            put32(tls+0x88u,0x10u);put32(tls+0x8Cu,link);
            return 1;
        }
        if(command==0x08050080u) {
            uint64_t size=(uint64_t)mem32(tls+0x84u)|((uint64_t)mem32(tls+0x88u)<<32);
            if(size>0x4000000u)error=0xD9004676u;else if(ftruncate(fileno(file),(off_t)size))error=native_fs_error();
        } else if(command==0x08080000u) {
            /* Close: drop this handle; only fclose() the FILE* when no other
               handle (e.g. an OpenLinkFile alias) still references it. */
            native_files[handle]=NULL;
            int shared=0;
            for(unsigned i=0;i<256u;++i) if(native_files[i]==file){shared=1;break;}
            if(!shared && fclose(file))error=native_fs_error();
            if(!shared) { native_file_paths[handle][0]=0; native_file_modes[handle]=0; }
        }
        else if(command==0x08090000u) { if(fflush(file))error=native_fs_error(); }
        else return 0;
        native_fs_reply(command,0x40u,error);return 1;
    }
    if(handle<256u && native_dirs[handle]) {
        if(command==0x08020000u) { closedir(native_dirs[handle]);native_dirs[handle]=NULL;guest_dir_open[handle]=0;native_fs_reply(command,0x40u,0);return 1; }
        if(command==0x08010042u) {
            uint32_t count=mem32(tls+0x84u),desc=mem32(tls+0x88u),ptr=mem32(tls+0x8Cu),mapped,read=0,error=0;
            if(count>1024u || !guest_memory_range(ptr,(uint64_t)count*0x228u,&mapped))error=0xD9004676u;
            else while(read<count) {
                struct dirent *entry=readdir(native_dirs[handle]);if(!entry)break;
                if(!strcmp(entry->d_name,".") || !strcmp(entry->d_name,".."))continue;
                uint32_t target=ptr+read*0x228u;memset(ram+mapped+read*0x228u,0,0x228u);
                unsigned len=0;while(entry->d_name[len] && len<0x105u) { put16(target+len*2u,(uint8_t)entry->d_name[len]);++len; }
                char path[4096];struct stat st;snprintf(path,sizeof(path),"%s/%s",native_dir_paths[handle],entry->d_name);
                if(!stat(path,&st)) { put8(target+0x21Cu,S_ISDIR(st.st_mode));put32(target+0x220u,(uint32_t)st.st_size);put32(target+0x224u,(uint32_t)((uint64_t)st.st_size>>32)); }
                ++read;
            }
            native_fs_reply(command,0x82u,error);put32(tls+0x88u,read);put32(tls+0x8Cu,desc);put32(tls+0x90u,ptr);return 1;
        }
    }
    return 0;
}
