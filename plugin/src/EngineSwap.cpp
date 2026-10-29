#include "EngineSwap.h"
#include "EngineCensus.generated.h"

extern "C" {
#include "engine.h"   // Phase 0 wrapper (engine/engine.h)
}

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if defined (__linux__)
 #include <dlfcn.h>
 #include <elf.h>
#endif

// True addresses of three unmistakable engine globals (the firmware's own
// symbols, linked into every host binary) — the symbol table's addresses
// for these names must reproduce these exactly or the table is rejected.
extern "C"
{
    extern char seq_patternSet;
    extern char fxBus;
    extern char parameterArray;
}

EngineSwap& EngineSwap::instance()
{
    static EngineSwap s;
    return s;
}

bool EngineSwap::available()
{
    if (! attempted)
    {
        attempted = true;
        ok = buildTable();
        if (! ok)
            std::fprintf (stderr, "ElfDrumLXR: per-instance swap unavailable "
                                  "(symbol census mismatch) - singleton mode\n");
    }
    return ok;
}

void EngineSwap::saveTo (std::vector<uint8_t>& blob) const
{
    blob.resize (total);
    for (const auto& s : segs)
        std::memcpy (blob.data() + s.offset, (const void*) s.address, s.size);
}

void EngineSwap::loadFrom (const std::vector<uint8_t>& blob) const
{
    if (blob.size() != total) return;
    for (const auto& s : segs)
        std::memcpy ((void*) s.address, blob.data() + s.offset, s.size);
}

void EngineSwap::capturePristineIfNeeded()
{
    if (pristineBlob.empty() && ok)
        saveTo (pristineBlob);
}

#if defined (__linux__)
namespace {

struct SymInfo { uintptr_t value; size_t size; bool writableData; };

// Parse the ELF symbol table of the module containing &lxr_init. Returns
// every symbol grouped by name (file-local statics legitimately repeat a
// name), and the load bias that maps st_value to a runtime address —
// derived from lxr_init itself, so ET_EXEC/ET_DYN quirks cancel out.
bool readModuleSymbols (std::map<std::string, std::vector<SymInfo>>& out, uintptr_t& bias)
{
    Dl_info info {};
    if (dladdr ((const void*) &lxr_init, &info) == 0 || info.dli_fname == nullptr)
        return false;

    FILE* f = std::fopen (info.dli_fname, "rb");
    if (f == nullptr) return false;
    std::fseek (f, 0, SEEK_END);
    const long len = std::ftell (f);
    std::fseek (f, 0, SEEK_SET);
    if (len <= 0) { std::fclose (f); return false; }
    std::vector<uint8_t> file ((size_t) len);
    const bool readOk = std::fread (file.data(), 1, file.size(), f) == file.size();
    std::fclose (f);
    if (! readOk || file.size() < sizeof (Elf64_Ehdr)) return false;

    const auto* eh = (const Elf64_Ehdr*) file.data();
    if (std::memcmp (eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64)
        return false;
    if (eh->e_shoff == 0 || eh->e_shentsize < sizeof (Elf64_Shdr)) return false;
    const auto* sh = (const Elf64_Shdr*) (file.data() + eh->e_shoff);

    const Elf64_Shdr* symtab = nullptr;
    const Elf64_Shdr* strtab = nullptr;
    for (int i = 0; i < eh->e_shnum; ++i)
        if (sh[i].sh_type == SHT_SYMTAB)
        {
            symtab = &sh[i];
            if (sh[i].sh_link < (uint32_t) eh->e_shnum) strtab = &sh[sh[i].sh_link];
            break;
        }
    if (symtab == nullptr || strtab == nullptr) return false;
    if (symtab->sh_offset + symtab->sh_size > file.size()
        || strtab->sh_offset + strtab->sh_size > file.size()) return false;

    std::vector<bool> writable ((size_t) eh->e_shnum, false);
    for (int i = 0; i < eh->e_shnum; ++i)
        writable[(size_t) i] = (sh[i].sh_flags & (SHF_WRITE | SHF_ALLOC))
                                    == (SHF_WRITE | SHF_ALLOC);

    const auto* syms = (const Elf64_Sym*) (file.data() + symtab->sh_offset);
    const char* strs = (const char*) (file.data() + strtab->sh_offset);
    const size_t count = symtab->sh_size / sizeof (Elf64_Sym);
    for (size_t i = 0; i < count; ++i)
    {
        const Elf64_Sym& s = syms[i];
        if (s.st_name == 0 || s.st_name >= strtab->sh_size) continue;
        const bool wd = s.st_shndx < eh->e_shnum && writable[s.st_shndx]
                        && ELF64_ST_TYPE (s.st_info) == STT_OBJECT;
        out[strs + s.st_name].push_back ({ (uintptr_t) s.st_value, (size_t) s.st_size, wd });
    }

    auto it = out.find ("lxr_init");
    if (it == out.end() || it->second.empty()) return false;
    bias = (uintptr_t) &lxr_init - it->second[0].value;
    return true;
}

} // namespace
#endif

bool EngineSwap::buildTable()
{
#if defined (__linux__)
    std::map<std::string, std::vector<SymInfo>> byName;
    uintptr_t bias = 0;
    if (! readModuleSymbols (byName, bias)) return false;

    // (1) Anchor validation: three big engine globals must land exactly on
    // their true addresses.
    const struct { const char* name; const void* truth; } anchors[] = {
        { "seq_patternSet", (const void*) &seq_patternSet },
        { "fxBus",          (const void*) &fxBus },
        { "parameterArray", (const void*) &parameterArray },
    };
    for (const auto& a : anchors)
    {
        auto it = byName.find (a.name);
        if (it == byName.end()) return false;
        bool found = false;
        for (const auto& s : it->second)
            if (s.writableData && bias + s.value == (uintptr_t) a.truth) found = true;
        if (! found) return false;
    }

    // (2) Census multiset: every (name, size) entry in the committed census
    // must consume a distinct writable-data symbol of the same name+size.
    std::map<std::pair<std::string, size_t>, std::vector<uintptr_t>> pool;
    for (const auto& [name, list] : byName)
        for (const auto& s : list)
            if (s.writableData)
                pool[{ name, s.size }].push_back (bias + s.value);
    for (auto& [key, addrs] : pool)
        std::sort (addrs.begin(), addrs.end());

    std::vector<Segment> matched;
    size_t offset = 0;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (size_t i = 0; i < kEngineCensusCount; ++i)
    {
        const auto key = std::make_pair (std::string (kEngineCensus[i].name),
                                         kEngineCensus[i].size);
        auto it = pool.find (key);
        if (it == pool.end() || it->second.empty()) return false;
        const uintptr_t addr = it->second.front();
        it->second.erase (it->second.begin());
        matched.push_back ({ addr, offset, kEngineCensus[i].size });
        offset += kEngineCensus[i].size;
        lo = std::min (lo, addr);
        hi = std::max (hi, addr + kEngineCensus[i].size);
    }
    if (offset != kEngineCensusBytes) return false;

    // (3) Span sanity: the engine's data clusters in one region of the
    // module (the archive's objects link together). A same-name impostor
    // from another library would land far outside it.
    if (hi - lo > 16u * 1024u * 1024u) return false;

    segs = std::move (matched);
    total = offset;
    return true;
#else
    return false; // Mach-O / PE symbol walking is future work — singleton.
#endif
}
