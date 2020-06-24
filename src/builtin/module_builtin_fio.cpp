#include "daScript/misc/platform.h"

#include "module_builtin.h"

#include "daScript/simulate/aot_builtin_fio.h"

#include "daScript/simulate/simulate_nodes.h"
#include "daScript/ast/ast_interop.h"
#include "daScript/ast/ast_policy_types.h"
#include "daScript/ast/ast_handle.h"
#include "daScript/simulate/aot_builtin_time.h"

#include "daScript/misc/performance_time.h"

MAKE_TYPE_FACTORY(clock, das::Time)// use MAKE_TYPE_FACTORY out of namespace. Some compilers not happy otherwise

#if _WIN32
    /* macro definitions extracted from git/git-compat-util.h */
    #define PROT_READ  1
    #define PROT_WRITE 2
    #define MAP_FAILED ((void*)-1)
    /* macro definitions extracted from /usr/include/bits/mman.h */
    #define MAP_SHARED  0x01        /* Share changes.  */
    #define MAP_PRIVATE 0x02        /* Changes are private.  */
    void* mmap(void* start, size_t length, int prot, int flags, int fd, off_t offset);
    int munmap(void* start, size_t length);
#endif

namespace das {

    IMPLEMENT_OP2_EVAL_BOOL_POLICY(Equ,Time);
    IMPLEMENT_OP2_EVAL_BOOL_POLICY(NotEqu,Time);
    IMPLEMENT_OP2_EVAL_BOOL_POLICY(GtEqu,Time);
    IMPLEMENT_OP2_EVAL_BOOL_POLICY(LessEqu,Time);
    IMPLEMENT_OP2_EVAL_BOOL_POLICY(Gt,Time);
    IMPLEMENT_OP2_EVAL_BOOL_POLICY(Less,Time);
    IMPLEMENT_OP2_EVAL_POLICY(Sub, Time);

    struct TimeAnnotation : ManagedValueAnnotation<Time> {
        TimeAnnotation() : ManagedValueAnnotation<Time>("clock","das::Time") {}
        virtual void walk ( DataWalker & walker, void * data ) override {
            if ( walker.reading ) {
                // there shuld be a way to read time from the stream here
            } else {
                Time * t = (Time *) data;
                char mbstr[100];
                if ( strftime(mbstr, sizeof(mbstr), "%c", localtime(&t->time)) ) {
                    char * str = mbstr;
                    walker.String(str);
                }
            }
        }
    };

    Time builtin_clock() {
        Time t;
        t.time = time(nullptr);
        return t;
    }

    void Module_BuiltIn::addTime(ModuleLibrary & lib) {
        addAnnotation(make_smart<TimeAnnotation>());
        addFunctionBasic<Time>(*this,lib);
        addFunctionOrdered<Time>(*this,lib);
        addFunction( make_smart<BuiltInFn<Sim_Sub<Time>,float,Time,Time>>("-",lib,"Sub"));
        addExtern<DAS_BIND_FUN(builtin_clock)>(*this, lib, "get_clock", SideEffects::modifyExternal, "builtin_clock");
    }
}



#if !DAS_NO_FILEIO

#include <sys/stat.h>

#if defined(_MSC_VER)

#include <io.h>
#include <direct.h>

#else
#include <libgen.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/mman.h>
#endif

namespace das {
    struct FStat {
        struct stat stats;
        bool        is_valid;
        uint64_t size() const   { return stats.st_size; }
#if defined(_MSC_VER)
        Time     atime() const  { return stats.st_atime; }
        Time     ctime() const  { return stats.st_ctime; }
        Time     mtime() const  { return stats.st_mtime; }
        bool     is_reg() const { return stats.st_mode & _S_IFREG; }
        bool     is_dir() const { return stats.st_mode & _S_IFDIR; }

#else
        Time     atime() const  { return stats.st_atime; }
        Time     ctime() const  { return stats.st_ctime; }
        Time     mtime() const  { return stats.st_mtime; }
        bool     is_reg() const { return S_ISREG(stats.st_mode); }
        bool     is_dir() const { return S_ISDIR(stats.st_mode); }

#endif
    };
}


MAKE_TYPE_FACTORY(FStat, das::FStat)
MAKE_TYPE_FACTORY(FILE,FILE)

namespace das {
    void builtin_sleep ( uint32_t msec ) {
#if defined(_MSC_VER)
        _sleep(msec);
#else
        usleep(msec);
#endif
    }

    #include "fio.das.inc"

    struct FStatAnnotation : ManagedStructureAnnotation <FStat,true> {
        FStatAnnotation(ModuleLibrary & ml) : ManagedStructureAnnotation ("FStat", ml) {
            addField<DAS_BIND_MANAGED_FIELD(is_valid)>("is_valid");
            addProperty<DAS_BIND_MANAGED_PROP(size)>("size");
            addProperty<DAS_BIND_MANAGED_PROP(atime)>("atime");
            addProperty<DAS_BIND_MANAGED_PROP(ctime)>("ctime");
            addProperty<DAS_BIND_MANAGED_PROP(mtime)>("mtime");
            addProperty<DAS_BIND_MANAGED_PROP(is_reg)>("is_reg");
            addProperty<DAS_BIND_MANAGED_PROP(is_dir)>("is_dir");
        }
        virtual bool canMove() const override { return true; }
        virtual bool canCopy() const override { return true; }
        virtual bool isLocal() const override { return true; }
    };

    struct FileAnnotation : ManagedStructureAnnotation <FILE,false> {
        FileAnnotation(ModuleLibrary & ml) : ManagedStructureAnnotation ("FILE", ml) {
        }
    };


    void builtin_fprint ( const FILE * f, const char * text, Context * context ) {
        if ( !f ) context->throw_error("can't fprint NULL");
        if ( text ) fputs(text,(FILE *)f);
    }

    const FILE * builtin_fopen  ( const char * name, const char * mode ) {
        if ( name && mode ) {
            FILE * f = fopen(name, mode);
            if ( f ) setvbuf(f, NULL, _IOFBF, 65536);
            return f;
        } else {
            return nullptr;
        }
    }

    void builtin_fclose ( const FILE * f, Context * context ) {
        if ( !f ) context->throw_error("can't fclose NULL");
        fclose((FILE *)f);
    }

    const FILE * builtin_stdin() {
        return stdin;
    }

    const FILE * builtin_stdout() {
        return stdout;
    }

    const FILE * builtin_stderr() {
        return stderr;
    }

    bool builtin_feof(const FILE* _f) {
        FILE* f = (FILE*)_f;
        return feof(f);
    }

    void builtin_map_file(const FILE* f, const TBlock<void, TTemporary<const char*>>& blk, Context* context) {
        if ( !f ) context->throw_error("can't map NULL file");
        struct stat st;
        int fd = fileno((FILE *)f);
        fstat(fd, &st);
        void* data = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        vec4f args[1];
        args[0] = cast<void*>::from(data);
        context->invoke(blk, args, nullptr);
        munmap(data, 0);
    }

    char * builtin_fread ( const FILE * f, Context * context ) {
        if ( !f ) context->throw_error("can't fread NULL");
        struct stat st;
        int fd = fileno((FILE*)f);
        fstat(fd, &st);
        char * res = context->stringHeap->allocateString(nullptr, st.st_size);
        fseek((FILE*)f, 0, SEEK_SET);
        auto bytes = fread(res, 1, st.st_size, (FILE *)f);
        if ( bytes != st.st_size ) context->throw_error_ex("uncorrect fread result, expected %d, got %d bytes", st.st_size, bytes);
        return res;
    }

    char * builtin_fgets(const FILE* f, Context* context) {
        if ( !f ) context->throw_error("can't fgets NULL");
        char buffer[1024];
        if (char* buf = fgets(buffer, sizeof(buffer), (FILE *)f)) {
            return context->stringHeap->allocateString(buf, uint32_t(strlen(buf)));
        } else {
            return nullptr;
        }
    }

    void builtin_fwrite ( const FILE * f, char * str, Context * context ) {
        if ( !f ) context->throw_error("can't fprint NULL");
        if (!str) return;
        uint32_t len = stringLength(*context, str);
        if (len) fwrite(str, 1, len, (FILE*)f);
    }

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable:4100)
#endif

    vec4f builtin_read ( Context & context, SimNode_CallBase * call, vec4f * args )
    {
        DAS_ASSERT ( call->types[1]->isRef() || call->types[1]->isRefType() || call->types[1]->type==Type::tString);
        auto fp = cast<FILE *>::to(args[0]);
        if ( !fp ) context.throw_error("can't read NULL");
        auto buf = cast<void *>::to(args[1]);
        auto len = cast<int32_t>::to(args[2]);
        int32_t res = (int32_t) fread(buf,1,len,fp);
        return cast<int32_t>::from(res);
    }

    vec4f builtin_write ( Context & context, SimNode_CallBase * call, vec4f * args )
    {
        DAS_ASSERT ( call->types[1]->isRef() || call->types[1]->isRefType() || call->types[1]->type==Type::tString);
        auto fp = cast<FILE *>::to(args[0]);
        if ( !fp ) context.throw_error("can't write NULL");
        auto buf = cast<void *>::to(args[1]);
        auto len = cast<int32_t>::to(args[2]);
        int32_t res = (int32_t) fwrite(buf,1,len,fp);
        return cast<int32_t>::from(res);
    }

#ifdef _MSC_VER
#pragma warning(pop)
#endif

    // loads(file,block<data>)
    vec4f builtin_load ( Context & context, SimNode_CallBase *, vec4f * args ) {
        auto fp = cast<FILE *>::to(args[0]);
        if ( !fp ) context.throw_error("can't load NULL");
        int32_t len = cast<int32_t>::to(args[1]);
        if (len < 0) context.throw_error_ex("can't read negative number from binary save, %d", len);
        Block * block = cast<Block *>::to(args[2]);
        char * buf = (char *) malloc(len + 1);
        if (!buf) context.throw_error_ex("can't read. out of memory, %d", len);
        vec4f bargs[1];
        int32_t rlen = int32_t(fread(buf, 1, len, fp));
        if ( rlen != len ) {
            bargs[0] = v_zero();
            context.invoke(*block, bargs, nullptr);
        }  else {
            buf[rlen] = 0;
            bargs[0] = cast<char *>::from(buf);
            context.invoke(*block, bargs, nullptr);
        }
        free(buf);
        return v_zero();
    }


    char * builtin_dirname ( const char * name, Context * context ) {
        if ( name ) {
#if defined(_MSC_VER)
            char full_path[ _MAX_PATH ];
            char dir[ _MAX_DIR ];
            char fname[ _MAX_FNAME ];
            char ext[ _MAX_EXT ];
            _splitpath(name, full_path, dir, fname, ext);
            strcat(full_path, dir);
            uint32_t len = uint32_t(strlen(full_path));
            if (len) {
                if (full_path[len - 1] == '/' || full_path[len - 1] == '\\') {
                    full_path[--len] = 0;
                }
            }
            return context->stringHeap->allocateString(full_path, len);
#else
            char * tempName = strdup(name);
            char * dirName = dirname(tempName);
            char * result = context->stringHeap->allocateString(dirName, strlen(dirName));
            free(tempName);
            return result;
#endif
        } else {
            return nullptr;
        }
    }

    char * builtin_basename ( const char * name, Context * context ) {
        if ( name ) {
#if defined(_MSC_VER)
            char drive[ _MAX_DRIVE ];
            char full_path[ _MAX_PATH ];
            char dir[ _MAX_DIR ];
            char ext[ _MAX_EXT ];
            _splitpath(name, drive, dir, full_path, ext);
            strcat(full_path, ext);
            return context->stringHeap->allocateString(full_path, uint32_t(strlen(full_path)));
#else
            char * tempName = strdup(name);
            char * dirName = basename(tempName);
            char * result = context->stringHeap->allocateString(dirName, strlen(dirName));
            free(tempName);
            return result;
#endif
        } else {
            return nullptr;
        }
    }

    bool builtin_fstat ( const FILE * f, FStat & fs ) {
        return fstat(fileno((FILE *)f), &fs.stats) == 0;
    }

    bool builtin_stat ( const char * filename, FStat & fs ) {
        if ( filename!=nullptr ) {
            return stat(filename, &fs.stats) == 0;
        } else {
            return false;
        }
    }

     void builtin_dir ( const char * path, const Block & fblk, Context * context ) {
#if defined(_MSC_VER)
        _finddata_t c_file;
        intptr_t hFile;
        string findPath = string(path) + "/*";
        if ((hFile = _findfirst(findPath.c_str(), &c_file)) != -1L) {
            do {
                char * fname = context->stringHeap->allocateString(c_file.name, uint32_t(strlen(c_file.name)));
                vec4f args[1] = {
                    cast<char *>::from(fname)
                };
                context->invoke(fblk, args, nullptr);
            } while (_findnext(hFile, &c_file) == 0);
        }
        _findclose(hFile);
 #else
        DIR *dir;
        struct dirent *ent;
        if ((dir = opendir (path)) != NULL) {
            while ((ent = readdir (dir)) != NULL) {
                char * fname = context->stringHeap->allocateString(ent->d_name,uint32_t(strlen(ent->d_name)));
                vec4f args[1] = {
                    cast<char *>::from(fname)
                };
                context->invoke(fblk, args, nullptr);
            }
            closedir (dir);
        }
 #endif
    }

    bool builtin_mkdir ( const char * path ) {
        if ( path ) {
#if defined(_MSC_VER)
            return _mkdir(path) == 0;
#else
            return mkdir(path, ACCESSPERMS) == 0;
#endif
        } else {
            return false;
        }
    }

    class Module_FIO : public Module {
    public:
        Module_FIO() : Module("fio") {
            DAS_PROFILE_SECTION("Module_FIO");
            ModuleLibrary lib;
            lib.addModule(this);
            lib.addBuiltInModule();
            // type
            addAnnotation(make_smart<FileAnnotation>(lib));
            addAnnotation(make_smart<FStatAnnotation>(lib));
            // file io
            addExtern<DAS_BIND_FUN(builtin_fopen)>(*this, lib, "fopen", SideEffects::modifyExternal, "builtin_fopen");
            addExtern<DAS_BIND_FUN(builtin_fclose)>(*this, lib, "fclose", SideEffects::modifyExternal, "builtin_fclose");
            addExtern<DAS_BIND_FUN(builtin_fprint)>(*this, lib, "fprint", SideEffects::modifyExternal, "builtin_fprint");
            addExtern<DAS_BIND_FUN(builtin_fread)>(*this, lib, "fread", SideEffects::modifyExternal, "builtin_fread");
            addExtern<DAS_BIND_FUN(builtin_map_file)>(*this, lib, "fmap", SideEffects::modifyExternal, "builtin_map_file");
            addExtern<DAS_BIND_FUN(builtin_fgets)>(*this, lib, "fgets", SideEffects::modifyExternal, "builtin_fgets");
            addExtern<DAS_BIND_FUN(builtin_fwrite)>(*this, lib, "fwrite", SideEffects::modifyExternal, "builtin_fwrite");
            addExtern<DAS_BIND_FUN(builtin_feof)>(*this, lib, "feof", SideEffects::modifyExternal, "builtin_feof");
            // builtin file functions
            addInterop<builtin_read,int,const FILE*,vec4f,int32_t>(*this, lib, "_builtin_read",SideEffects::modifyExternal, "builtin_read");
            addInterop<builtin_write,int,const FILE*,vec4f,int32_t>(*this, lib, "_builtin_write",SideEffects::modifyExternal, "builtin_write");
            addInterop<builtin_load,void,const FILE*,int32_t,const Block &>(*this, lib, "_builtin_load",das::SideEffects::modifyExternal, "builtin_load");
            addExtern<DAS_BIND_FUN(builtin_dirname)>(*this, lib, "dir_name", SideEffects::none, "builtin_dirname");
            addExtern<DAS_BIND_FUN(builtin_basename)>(*this, lib, "base_name", SideEffects::none, "builtin_basename");
            addExtern<DAS_BIND_FUN(builtin_fstat)>(*this, lib, "fstat", SideEffects::modifyExternal, "builtin_fstat");
            addExtern<DAS_BIND_FUN(builtin_stat)>(*this, lib, "stat", SideEffects::modifyExternal, "builtin_stat");
            addExtern<DAS_BIND_FUN(builtin_dir)>(*this, lib, "builtin_dir", SideEffects::modifyExternal, "builtin_dir");
            addExtern<DAS_BIND_FUN(builtin_mkdir)>(*this, lib, "mkdir", SideEffects::modifyExternal, "builtin_mkdir");
            addExtern<DAS_BIND_FUN(builtin_stdin)>(*this, lib, "fstdin", SideEffects::modifyExternal, "builtin_stdin");
            addExtern<DAS_BIND_FUN(builtin_stdout)>(*this, lib, "fstdout", SideEffects::modifyExternal, "builtin_stdout");
            addExtern<DAS_BIND_FUN(builtin_stderr)>(*this, lib, "fstderr", SideEffects::modifyExternal, "builtin_stderr");
            addExtern<DAS_BIND_FUN(builtin_sleep)>(*this, lib, "sleep", SideEffects::modifyExternal, "builtin_sleep");
            // add builtin module
            compileBuiltinModule("fio.das",fio_das, sizeof(fio_das));
            // lets verify all names
            uint32_t verifyFlags = uint32_t(VerifyBuiltinFlags::verifyAll);
            verifyFlags &= ~VerifyBuiltinFlags::verifyHandleTypes;  // we skip annotatins due to FILE and FStat
            verifyBuiltinNames(verifyFlags);
            // and now its aot ready
            verifyAotReady();
        }
        virtual ModuleAotType aotRequire ( TextWriter & tw ) const override {
            tw << "#include \"daScript/simulate/aot_builtin_fio.h\"\n";
            return ModuleAotType::cpp;
        }
    };
}

REGISTER_MODULE_IN_NAMESPACE(Module_FIO,das);

#if _WIN32

#include <windows.h>

void * mmap (void* start, size_t length, int /*prot*/, int /*flags*/, int fd, off_t offset) {
    HANDLE hmap;
    void* temp;
    size_t len;
    struct stat st;
    uint64_t o = offset;
    uint32_t l = o & 0xFFFFFFFF;
    uint32_t h = (o >> 32) & 0xFFFFFFFF;
    fstat(fd, &st);
    len = (size_t)st.st_size;
    if ((length + offset) > len)
        length = len - offset;
    hmap = CreateFileMapping((HANDLE)_get_osfhandle(fd), 0, PAGE_WRITECOPY, 0, 0, 0);
    if (!hmap)
        return MAP_FAILED;
    temp = MapViewOfFileEx(hmap, FILE_MAP_COPY, h, l, length, start);
    if (!CloseHandle(hmap))
        fprintf(stderr, "unable to close file mapping handle\n");
    return temp ? temp : MAP_FAILED;
}

int munmap ( void* start, size_t ) {
    return !UnmapViewOfFile(start);
}

#endif

#endif
