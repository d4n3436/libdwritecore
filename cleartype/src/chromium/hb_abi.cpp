//+--------------------------------------------------------------------------
//
//  hb_abi.cpp - reach HarfBuzz, however this build links it.
//
//  A build either links HarfBuzz as a shared library, where the loader binds
//  Blink's calls to the preloaded entry points in bold_shaping.cpp, or
//  compiles it in, where those entry points are never called and the function
//  has to be replaced where it stands. Which case it is decides how the swap
//  is installed, so it is settled once, at load.
//
//  A HarfBuzz the process does not already have is never loaded. Its objects
//  would be a second, unrelated set, and an hb_font_t from Blink's copy is not
//  a font to that one.
//
//  Names a static link kept out of the dynamic symbol table are read from the
//  symbol table the file holds on disk, which needs the filesystem and so runs
//  in the zygote, from ChromiumPatchInit.
//
//----------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hb_abi.h"

namespace hb_abi {
namespace {

using Table = std::unordered_map<std::string, void*>;

//----------------------------------------------------------------------------
// The symbol tables of the images the process has loaded.
//----------------------------------------------------------------------------

struct Image
{
    std::string path;
    uintptr_t bias;
    bool main;
};

// This library's own load address. It exports the very names being looked up,
// so leaving it in would answer every lookup with the interposing function and
// send a call meant for HarfBuzz straight back here.
uintptr_t Self()
{
    Dl_info info = {};
    if (dladdr(reinterpret_cast<const void*>(&Self), &info) == 0) {
        return 0;
    }
    return reinterpret_cast<uintptr_t>(info.dli_fbase);
}

int Note(dl_phdr_info* info, size_t, void* out)
{
    auto* images = static_cast<std::vector<Image>*>(out);
    static const uintptr_t self = Self();
    if (info->dlpi_addr == self) {
        return 0;
    }
    // The main executable comes through with an empty name.
    const char* name = info->dlpi_name;
    const bool main_image = name == nullptr || name[0] == '\0';
    images->push_back({main_image ? "/proc/self/exe" : name,
                       static_cast<uintptr_t>(info->dlpi_addr), main_image});
    return 0;
}

// A file mapped for as long as it is being read.
class Mapping
{
  public:
    explicit Mapping(const std::string& path)
    {
        const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            return;
        }
        struct stat st = {};
        if (fstat(fd, &st) == 0 && st.st_size > 0) {
            size_ = static_cast<size_t>(st.st_size);
            void* at = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
            if (at != MAP_FAILED) {
                base_ = static_cast<const unsigned char*>(at);
            } else {
                size_ = 0;
            }
        }
        close(fd);
    }
    ~Mapping()
    {
        if (base_ != nullptr) {
            munmap(const_cast<unsigned char*>(base_), size_);
        }
    }
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;

    // A struct wholly inside the file, or null.
    template <typename T>
    const T* At(const uint64_t offset, const size_t count = 1) const
    {
        if (base_ == nullptr || count == 0 || offset > size_ ||
            sizeof(T) * count > size_ - offset) {
            return nullptr;
        }
        return reinterpret_cast<const T*>(base_ + offset);
    }

  private:
    const unsigned char* base_ = nullptr;
    size_t size_ = 0;
};

// One symbol table and the strings it names its symbols from.
void ReadTable(const Mapping& file, const Elf64_Shdr& symbols, const Elf64_Shdr& strings,
               const uintptr_t bias, const char* prefix, const size_t prefix_len, Table* out)
{
    if (symbols.sh_entsize != sizeof(Elf64_Sym) || symbols.sh_size == 0) {
        return;
    }
    const size_t count = symbols.sh_size / sizeof(Elf64_Sym);
    const auto* sym = file.At<Elf64_Sym>(symbols.sh_offset, count);
    const auto* text = file.At<char>(strings.sh_offset, strings.sh_size);
    if (sym == nullptr || text == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        if (ELF64_ST_TYPE(sym[i].st_info) != STT_FUNC || sym[i].st_value == 0 ||
            sym[i].st_shndx == SHN_UNDEF || sym[i].st_name >= strings.sh_size) {
            continue;
        }
        // The section is mapped whole, so a name running to its end is still
        // terminated by the section's own trailing zero.
        const char* name = text + sym[i].st_name;
        if (std::strncmp(name, prefix, prefix_len) != 0) {
            continue;
        }
        out->emplace(name, reinterpret_cast<void*>(bias + sym[i].st_value));
    }
}

void ReadImage(const Image& image, const char* prefix, const size_t prefix_len, Table* out)
{
    const Mapping file(image.path);
    const auto* header = file.At<Elf64_Ehdr>(0);
    if (header == nullptr || std::memcmp(header->e_ident, ELFMAG, SELFMAG) != 0 ||
        header->e_ident[EI_CLASS] != ELFCLASS64 || header->e_shoff == 0 ||
        header->e_shentsize != sizeof(Elf64_Shdr)) {
        return;
    }
    const auto* sections = file.At<Elf64_Shdr>(header->e_shoff, header->e_shnum);
    if (sections == nullptr) {
        return;
    }
    // .symtab first: a static link leaves its names there and nowhere else,
    // and where both tables hold a name they agree on it.
    for (const uint32_t want : {uint32_t{SHT_SYMTAB}, uint32_t{SHT_DYNSYM}}) {
        for (unsigned i = 0; i < header->e_shnum; ++i) {
            if (sections[i].sh_type != want || sections[i].sh_link >= header->e_shnum) {
                continue;
            }
            ReadTable(file, sections[i], sections[sections[i].sh_link], image.bias, prefix,
                      prefix_len, out);
        }
    }
}

// Every defined function in the process's own images whose name starts with
// prefix, as name -> runtime address. The first definition of a name wins,
// which is the one the loader would have bound to.
Table Collect(const char* prefix, const bool main_only)
{
    Table out;
    std::vector<Image> images;
    dl_iterate_phdr(&Note, &images);
    const size_t prefix_len = std::strlen(prefix);
    for (const Image& image : images) {
        if (main_only && !image.main) {
            continue;
        }
        ReadImage(image, prefix, prefix_len, &out);
    }
    return out;
}

}  // namespace

std::unordered_map<std::string, void*> SymbolsWithPrefix(const char* prefix)
{
    return Collect(prefix, false);
}

namespace {

//----------------------------------------------------------------------------
// Which case this build is.
//----------------------------------------------------------------------------

// Whether the main executable's own dynamic symbol table names `symbol` as an
// import. dlsym cannot answer: this library exports the HarfBuzz entry points
// it interposes, so the global scope always has them.
bool ImageImports(const char* symbol)
{
    struct Ask
    {
        const char* symbol;
        bool found;
    } ask{symbol, false};

    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) {
            // The main image is the one with an empty name.
            if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') {
                return 0;
            }
            auto* a = static_cast<Ask*>(data);
            for (int i = 0; i < info->dlpi_phnum; ++i) {
                if (info->dlpi_phdr[i].p_type != PT_DYNAMIC) {
                    continue;
                }
                const auto* dyn = reinterpret_cast<const ElfW(Dyn)*>(
                    info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
                const char* strtab = nullptr;
                const ElfW(Sym)* symtab = nullptr;
                const uint32_t* hash = nullptr;
                const uint32_t* gnu_hash = nullptr;
                for (const ElfW(Dyn)* d = dyn; d->d_tag != DT_NULL; ++d) {
                    if (d->d_tag == DT_STRTAB) {
                        strtab = reinterpret_cast<const char*>(d->d_un.d_ptr);
                    } else if (d->d_tag == DT_SYMTAB) {
                        symtab = reinterpret_cast<const ElfW(Sym)*>(d->d_un.d_ptr);
                    } else if (d->d_tag == DT_HASH) {
                        hash = reinterpret_cast<const uint32_t*>(d->d_un.d_ptr);
                    } else if (d->d_tag == DT_GNU_HASH) {
                        gnu_hash = reinterpret_cast<const uint32_t*>(d->d_un.d_ptr);
                    }
                }
                if (strtab == nullptr || symtab == nullptr) {
                    continue;
                }
                // A bound is required: the tables are not promised to be
                // adjacent, so walking to the string table reads past the end
                // and faults. DT_HASH's second word is the symbol count.
                // DT_GNU_HASH, which is all a linker emits by default now,
                // hashes only the defined symbols and its second word is the
                // index the first of them sits at, so every import is below
                // that, which is the whole range this asks about.
                unsigned bound = 0;
                if (hash != nullptr) {
                    bound = hash[1];
                } else if (gnu_hash != nullptr) {
                    bound = gnu_hash[1];
                } else {
                    continue;
                }
                const ElfW(Sym)* end = symtab + bound;
                for (const ElfW(Sym)* sym = symtab; sym < end; ++sym) {
                    if (sym->st_shndx == SHN_UNDEF && sym->st_name != 0 &&
                        std::strcmp(strtab + sym->st_name, a->symbol) == 0) {
                        a->found = true;
                        return 1;
                    }
                }
            }
            return 1;
        },
        &ask);
    return ask.found;
}

// A build either has HarfBuzz or does not, so one name settles the question
// and nothing else is looked up until it is found.
constexpr const char* kWitness = "hb_shape";

Linkage g_where = Linkage::kAbsent;
bool g_resolved = false;

// A copy already in the process, which RTLD_NEXT misses when this library was
// loaded ahead of it. NOLOAD only, so nothing new is brought in.
void* Beside()
{
    static void* handle =
        dlopen("libharfbuzz.so.0", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    return handle;
}

Table& Symbols()
{
    static Table table;
    return table;
}

void Say(const char* what)
{
    if (std::getenv("DWC_BOLD_SHAPING_LOG") != nullptr) {
        (void)std::fprintf(stderr, "chromium-patch: bold shaping: %s\n", what);
    }
}

}  // namespace

void ResolveAtLoad()
{
    if (g_resolved) {
        return;
    }
    g_resolved = true;

    // Forces the compiled-in route on a build that links HarfBuzz shared,
    // which exercises the replacement against the interposed one. Only Blink's
    // call site differs between them.
    const bool as_static = std::getenv("DWC_BOLD_SHAPING_STATIC") != nullptr;

    // The executable's own imports decide this, not the global scope. Other
    // libraries in the process carry HarfBuzz for their own drawing, and
    // interposing for one of those reaches none of Blink's shaping while
    // handing this library hb_font_t objects from a HarfBuzz whose layout it
    // never probed.
    if (!as_static && ImageImports(kWitness) &&
        (dlsym(RTLD_NEXT, kWitness) != nullptr ||
         (Beside() != nullptr && dlsym(Beside(), kWitness) != nullptr))) {
        g_where = Linkage::kInterposable;
        Say("HarfBuzz is a shared library, so the interposed entry points are "
            "the ones Blink calls");
        return;
    }

    // The main image alone. A HarfBuzz loaded beside the executable names the
    // same symbols, and detouring one of those patches a copy Blink never
    // calls.
    Symbols() = Collect("hb_", true);
    if (Symbols().count(kWitness) != 0) {
        g_where = Linkage::kInImage;
        Say("HarfBuzz is compiled into the binary and its symbol table names "
            "it, so hb_shape is replaced where it stands");
        return;
    }

    g_where = Linkage::kAbsent;
    Say("HarfBuzz is compiled into the binary and stripped of its names, so "
        "shaping cannot be reached; a bold fallback run keeps the regular "
        "face's positioning");
}

bool ExecutableImports(const char* symbol)
{
    return ImageImports(symbol);
}

Linkage Where()
{
    return g_where;
}

void* Real(const char* name)
{
    if (name == nullptr) {
        return nullptr;
    }
    if (g_where == Linkage::kInImage) {
        const auto found = Symbols().find(name);
        return found == Symbols().end() ? nullptr : found->second;
    }
    if (void* fn = dlsym(RTLD_NEXT, name)) {
        return fn;
    }
    return Beside() != nullptr ? dlsym(Beside(), name) : nullptr;
}

}  // namespace hb_abi
