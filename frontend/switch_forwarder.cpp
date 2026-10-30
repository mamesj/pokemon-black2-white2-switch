// HOME Menu entries for the all-games program's games (switch_forwarder.h), made on the console:
// for each, a program NCA (the homebrew loader, tools/switch/forwarder, with the .nro's path and
// arguments in its romfs), a control NCA (the name and icon) and a meta NCA, written to the SD
// card's content storage and registered as an installed application.
//
// Adapted from sphaira's forwarder installer (https://github.com/ITotalJustice/sphaira,
// sphaira/source/owo.cpp, GPL-3.0; itself based on hacbrewpack's romfs creation): the same NCA
// layout, the header key made by the console (spl), no other keys. Like any forwarder, the
// entries need sigpatches to start.
#include "switch_forwarder.h"

#include <switch.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace SwitchForwarder
{
namespace
{
std::string g_debugFolder;
constexpr u32 IVFC_MAX_LEVEL = 6;
constexpr u32 IVFC_HASH_BLOCK_SIZE = 0x4000;
constexpr u32 PFS0_EXEFS_HASH_BLOCK_SIZE = 0x10000;
constexpr u32 PFS0_META_HASH_BLOCK_SIZE = 0x1000;
constexpr u32 PFS0_PADDING_SIZE = 0x200;
constexpr u32 ROMFS_ENTRY_EMPTY = 0xFFFFFFFF;
constexpr u32 ROMFS_FILEPARTITION_OFS = 0x200;
constexpr u32 NCA3_MAGIC = 0x3341434E;

// The header key's sources (the console turns them into the key: spl).
constexpr u8 HEADER_KEK_SRC[0x10] = { 0x1F, 0x12, 0x91, 0x3A, 0x4A, 0xCB, 0xF0, 0x0D, 0x4C, 0xDE, 0x3A, 0xF6, 0xD5, 0x23, 0x88, 0x2A };
constexpr u8 HEADER_KEY_SRC[0x20] = { 0x5A, 0x3E, 0xD8, 0x4F, 0xDE, 0xC0, 0xD8, 0x26, 0x31, 0xF7, 0xE2, 0x5D, 0x19, 0x7B, 0xF5, 0xD0,
                                      0x1C, 0x9B, 0x7B, 0xFA, 0xF6, 0x28, 0x18, 0x3D, 0x71, 0xF6, 0x4D, 0x73, 0xF1, 0x50, 0xB9, 0xD2 };

// ---------------------------------------------------------------------------
// The NCA's parts (as sphaira's yati/nx/nca.hpp).

#pragma pack(push, 1)
struct LayerRegion { u64 offset, size; };
struct HierarchicalSha256Data
{
    u8 master_hash[0x20];
    u32 block_size;
    u32 layer_count;
    LayerRegion hash_layer;
    LayerRegion pfs0_layer;
    LayerRegion unused_layers[3];
    u8 _0x78[0x80];
};
struct IvfcLevel { u64 logical_offset, hash_data_size; u32 block_size, reserved; };
struct IntegrityMetaInfo
{
    u32 magic, version, master_hash_size;
    u32 max_layers;
    IvfcLevel levels[6];
    u8 signature_salt[0x20];
    u8 master_hash[0x20];
    u8 _0xE0[0x18];
};
struct FsHeader
{
    u16 version;
    u8 fs_type, hash_type, encryption_type, metadata_hash_type;
    u8 _0x6[2];
    union
    {
        HierarchicalSha256Data sha256;
        IntegrityMetaInfo ivfc;
    } hash_data;
    u8 patch_info[0x40];
    u64 section_ctr;
    u8 spares_info[0x30];
    u8 compression_info[0x28];
    u8 meta_data_hash_data_info[0x30];
    u8 reserved[0x30];
};
struct SectionTableEntry { u32 media_start_offset, media_end_offset; u8 _0x8[4], _0xC[4]; };
struct NcaHeader
{
    u8 rsa_fixed_key[0x100];
    u8 rsa_npdm[0x100];
    u32 magic;
    u8 distribution_type, content_type, old_key_gen, kaek_index;
    u64 size;
    u64 program_id;
    u32 context_id;
    u32 sdk_version;
    u8 key_gen, sig_key_gen;
    u8 _0x222[0xE];
    u8 rights_id[0x10];
    SectionTableEntry fs_table[4];
    u8 fs_header_hash[4][0x20];
    u8 key_area[4][0x10];
    u8 _0x340[0xC0];
    FsHeader fs_header[4];
};
#pragma pack(pop)
static_assert(sizeof(HierarchicalSha256Data) == 0xF8, "");
static_assert(sizeof(IntegrityMetaInfo) == 0xF8, "");
static_assert(sizeof(FsHeader) == 0x200, "");
static_assert(sizeof(NcaHeader) == 0xC00, "");

enum { ContentType_Program = 0, ContentType_Meta = 1, ContentType_Control = 2 };
enum { FsType_RomFS = 0, FsType_PFS0 = 1 };
enum { HashType_Sha256 = 2, HashType_Ivfc = 3 };
enum { Encryption_None = 1 };

// NPDM (the program's permissions), as sphaira's yati/nx/npdm.hpp.
struct NpdmMeta
{
    u32 magic, signature_key_generation, _0x8;
    u8 flags, _0xD, main_thread_priority, main_thread_core_num;
    u32 _0x10, sys_resource_size, version, main_thread_stack_size;
    char title_name[0x10];
    char product_code[0x10];
    u8 _0x40[0x30];
    u32 aci0_offset, aci0_size, acid_offset, acid_size;
};
struct NpdmAcid
{
    u8 rsa_sig[0x100], rsa_pub[0x100];
    u32 magic, size;
    u8 version, _0x209[1], _0x20A[2];
    u32 flags;
    u64 program_id_min, program_id_max;
    u32 fac_offset, fac_size, sac_offset, sac_size, kac_offset, kac_size;
    u8 _0x238[8];
};
struct NpdmAci0
{
    u32 magic;
    u8 _0x4[0xC];
    u64 program_id;
    u8 _0x18[8];
    u32 fac_offset, fac_size, sac_offset, sac_size, kac_offset, kac_size;
    u8 _0x38[8];
};

struct CnmtHeader
{
    u64 title_id;
    u32 title_version;
    u8 meta_type, _0xD;
    NcmContentMetaHeader meta_header;
    u8 install_type, _0x17;
    u32 required_sys_version;
    u8 _0x1C[4];
};
static_assert(sizeof(CnmtHeader) == 0x20, "");
struct ContentMetaData
{
    NcmContentMetaHeader header;
    NcmApplicationMetaExtendedHeader extended;
    NcmContentInfo infos[3];
};
struct ContentStorageRecord
{
    NcmContentMetaKey key;
    u8 storage_id;
    u8 padding[7];
};

// ---------------------------------------------------------------------------
// Building.

struct Buf
{
    std::vector<u8> buf;
    u64 offset = 0;
    void Write(const void* data, u64 size)
    {
        if (offset + size > buf.size()) buf.resize(offset + size);
        memcpy(buf.data() + offset, data, size);
        offset += size;
    }
    void Seek(u64 to) { offset = to; }
    u64 Tell() const { return offset; }
};

struct FileEntry
{
    std::string name;
    std::vector<u8> data;
};
using Files = std::vector<FileEntry>;

void AddFile(Files& files, const char* name, const void* data, size_t size)
{
    FileEntry e;
    e.name = name;
    e.data.assign((const u8*)data, (const u8*)data + size);
    files.push_back(std::move(e));
}

u64 WritePadding(Buf& buf, u64 off, u64 block)
{
    const u64 size = block - (off % block);
    if (size)
    {
        std::vector<u8> zero(size);
        buf.Write(zero.data(), size);
    }
    return size;
}

u32 Align32(u32 v, u32 a) { return (v + (a - 1)) & ~(a - 1); }
u64 Align64(u64 v, u64 a) { return (v + (a - 1)) & ~(a - 1); }

u32 PathHash(u32 parent, const u8* path, u32 start, u32 len)
{
    u32 hash = parent ^ 123456789;
    for (u32 i = 0; i < len; i++)
    {
        hash = (hash >> 5) | (hash << 27);
        hash ^= path[start + i];
    }
    return hash;
}

u32 HashTableCount(u32 n)
{
    if (n < 3) return 3;
    if (n < 19) return n | 1;
    u32 c = n;
    while (c % 2 == 0 || c % 3 == 0 || c % 5 == 0 || c % 7 == 0 || c % 11 == 0 || c % 13 == 0 || c % 17 == 0) c++;
    return c;
}

// A romfs of files in its root folder ("/name"), as hacbrewpack makes one.
std::vector<u8> BuildRomfs(const Files& files, u64& outSize)
{
    const u32 dirTableSize = sizeof(romfs_dir);
    u32 fileTableSize = 0;
    for (const FileEntry& e : files) fileTableSize += sizeof(romfs_file) + Align32((u32)e.name.size() - 1, 4);
    const u32 dirHashCount = HashTableCount(1), fileHashCount = HashTableCount((u32)files.size());
    std::vector<u32> dirHash(dirHashCount, ROMFS_ENTRY_EMPTY), fileHash(fileHashCount, ROMFS_ENTRY_EMPTY);
    std::vector<u8> dirTable(dirTableSize), fileTable(fileTableSize);

    // The files, in order: their data offsets and table entries.
    std::vector<u64> dataOff(files.size());
    u64 partition = 0;
    u32 entryOff = 0;
    std::vector<u32> entryOffs(files.size());
    for (size_t i = 0; i < files.size(); i++)
    {
        partition = Align64(partition, 0x10);
        dataOff[i] = partition;
        partition += files[i].data.size();
        entryOffs[i] = entryOff;
        entryOff += sizeof(romfs_file) + Align32((u32)files[i].name.size() - 1, 4);
    }
    for (size_t i = 0; i < files.size(); i++)
    {
        romfs_file* f = (romfs_file*)(fileTable.data() + entryOffs[i]);
        f->parent = 0;
        f->sibling = i + 1 < files.size() ? entryOffs[i + 1] : ROMFS_ENTRY_EMPTY;
        f->dataOff = dataOff[i];
        f->dataSize = files[i].data.size();
        const u32 nameLen = (u32)files[i].name.size() - 1; // (without the leading /)
        const u32 hash = PathHash(0, (const u8*)files[i].name.c_str(), 1, nameLen);
        f->nextHash = fileHash[hash % fileHashCount];
        fileHash[hash % fileHashCount] = entryOffs[i];
        f->nameLen = nameLen;
        memcpy(f->name, files[i].name.c_str() + 1, nameLen);
    }
    {
        romfs_dir* d = (romfs_dir*)dirTable.data();
        d->parent = 0;
        d->sibling = ROMFS_ENTRY_EMPTY;
        d->childDir = ROMFS_ENTRY_EMPTY;
        d->childFile = files.empty() ? ROMFS_ENTRY_EMPTY : 0;
        const u32 hash = PathHash(0, nullptr, 0, 0);
        d->nextHash = dirHash[hash % dirHashCount];
        dirHash[hash % dirHashCount] = 0;
        d->nameLen = 0;
    }

    romfs_header header = {};
    header.headerSize = sizeof(header);
    header.fileHashTableSize = 4 * fileHashCount;
    header.fileTableSize = fileTableSize;
    header.dirHashTableSize = 4 * dirHashCount;
    header.dirTableSize = dirTableSize;
    header.fileDataOff = ROMFS_FILEPARTITION_OFS;
    header.dirHashTableOff = Align64(partition + ROMFS_FILEPARTITION_OFS, 4);
    header.dirTableOff = header.dirHashTableOff + header.dirHashTableSize;
    header.fileHashTableOff = header.dirTableOff + dirTableSize;
    header.fileTableOff = header.fileHashTableOff + header.fileHashTableSize;

    Buf buf;
    buf.Write(&header, sizeof(header));
    for (size_t i = 0; i < files.size(); i++)
    {
        buf.Seek(dataOff[i] + ROMFS_FILEPARTITION_OFS);
        buf.Write(files[i].data.data(), files[i].data.size());
    }
    buf.Seek(header.dirHashTableOff);
    buf.Write(dirHash.data(), header.dirHashTableSize);
    buf.Write(dirTable.data(), dirTableSize);
    buf.Write(fileHash.data(), header.fileHashTableSize);
    buf.Write(fileTable.data(), fileTableSize);
    buf.Seek(buf.buf.size());
    outSize = buf.Tell();
    WritePadding(buf, buf.Tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

std::vector<u8> BuildPfs0(const Files& files)
{
    struct { u32 magic, count, stringTableSize, pad; } header = {};
    struct Entry { u64 dataOffset, dataSize; u32 nameOffset, pad; };
    std::vector<Entry> table(files.size());
    std::vector<char> strings;
    u64 dataOff = 0;
    for (size_t i = 0; i < files.size(); i++)
    {
        table[i] = { dataOff, files[i].data.size(), (u32)strings.size(), 0 };
        strings.insert(strings.end(), files[i].name.begin(), files[i].name.end());
        strings.push_back(0);
        dataOff += files[i].data.size();
    }
    strings.resize((strings.size() + 0x1F) & ~size_t(0x1F));
    header.magic = 0x30534650;
    header.count = (u32)files.size();
    header.stringTableSize = (u32)strings.size();
    Buf buf;
    buf.Write(&header, sizeof(header));
    buf.Write(table.data(), sizeof(Entry) * table.size());
    buf.Write(strings.data(), strings.size());
    for (const FileEntry& e : files) buf.Write(e.data.data(), e.data.size());
    return buf.buf;
}

std::vector<u8> HashBlocks(const std::vector<u8>& data, u32 block, bool pad)
{
    Buf buf;
    u8 hash[SHA256_HASH_SIZE];
    for (size_t i = 0; i < data.size(); i += block)
    {
        sha256CalculateHash(hash, data.data() + i, std::min<size_t>(block, data.size() - i));
        buf.Write(hash, sizeof(hash));
    }
    if (pad) WritePadding(buf, buf.Tell(), IVFC_HASH_BLOCK_SIZE);
    return buf.buf;
}

void SectionEnd(NcaHeader& h, int index, u64 start, u64 end)
{
    h.fs_table[index].media_start_offset = (u32)(start / 0x200);
    h.fs_table[index].media_end_offset = (u32)(end / 0x200);
    h.fs_table[index]._0x8[0] = 1;
}

u64 SectionStart(const NcaHeader& h, int index)
{
    return index == 0 ? sizeof(NcaHeader) : (u64)h.fs_table[index - 1].media_end_offset * 0x200;
}

void WritePfs0Section(NcaHeader& h, int index, const Files& files, u32 block, Buf& buf)
{
    const std::vector<u8> pfs0 = BuildPfs0(files);
    const std::vector<u8> hashes = HashBlocks(pfs0, block, false);
    u8 master[SHA256_HASH_SIZE];
    sha256CalculateHash(master, hashes.data(), hashes.size());
    buf.Write(hashes.data(), hashes.size());
    const u64 pad = WritePadding(buf, hashes.size(), PFS0_PADDING_SIZE);
    FsHeader& fs = h.fs_header[index];
    fs.hash_data.sha256.pfs0_layer.offset = hashes.size() + pad;
    fs.hash_data.sha256.pfs0_layer.size = pfs0.size();
    buf.Write(pfs0.data(), pfs0.size());
    WritePadding(buf, buf.Tell(), 0x200);
    SectionEnd(h, index, SectionStart(h, index), buf.Tell());
    fs.hash_type = HashType_Sha256;
    fs.fs_type = FsType_PFS0;
    fs.version = 2;
    fs.hash_data.sha256.layer_count = 2;
    fs.hash_data.sha256.block_size = block;
    fs.encryption_type = Encryption_None;
    fs.hash_data.sha256.hash_layer.size = hashes.size();
    memcpy(fs.hash_data.sha256.master_hash, master, sizeof(master));
    sha256CalculateHash(h.fs_header_hash[index], &fs, sizeof(fs));
}

void WriteRomfsSection(NcaHeader& h, int index, const Files& files, Buf& buf)
{
    FsHeader& fs = h.fs_header[index];
    IntegrityMetaInfo& ivfcInfo = fs.hash_data.ivfc;
    std::vector<u8> ivfc[IVFC_MAX_LEVEL];
    ivfc[5] = BuildRomfs(files, ivfcInfo.levels[5].hash_data_size);
    for (int b = 4; b >= 0; b--)
    {
        ivfc[b] = HashBlocks(ivfc[b + 1], IVFC_HASH_BLOCK_SIZE, true);
        ivfcInfo.levels[b].hash_data_size = ivfc[b].size();
        ivfcInfo.levels[b].block_size = 0x0E;
    }
    ivfcInfo.levels[0].logical_offset = 0;
    for (int i = 1; i <= 5; i++) ivfcInfo.levels[i].logical_offset = ivfcInfo.levels[i - 1].logical_offset + ivfcInfo.levels[i - 1].hash_data_size;
    for (const auto& level : ivfc) buf.Write(level.data(), level.size());
    WritePadding(buf, buf.Tell(), 0x200);
    sha256CalculateHash(ivfcInfo.master_hash, ivfc[0].data(), ivfc[0].size());
    SectionEnd(h, index, SectionStart(h, index), buf.Tell());
    fs.hash_type = HashType_Ivfc;
    fs.fs_type = FsType_RomFS;
    fs.version = 2;
    ivfcInfo.magic = 0x43465649; // IVFC
    ivfcInfo.version = 0x20000;
    ivfcInfo.master_hash_size = SHA256_HASH_SIZE;
    ivfcInfo.max_layers = 7;
    fs.encryption_type = Encryption_None;
    ivfcInfo.levels[5].block_size = 0x0E;
    sha256CalculateHash(h.fs_header_hash[index], &fs, sizeof(fs));
}

void FinishHeader(NcaHeader& h, u64 tid, const u8* headerKey, u8 type, Buf& buf)
{
    h.magic = NCA3_MAGIC;
    h.distribution_type = 0; // (system)
    h.content_type = type;
    h.program_id = tid;
    h.sdk_version = 0x000C1100;
    h.size = buf.Tell();
    Aes128XtsContext ctx = {};
    aes128XtsContextCreate(&ctx, headerKey, headerKey + 0x10, true);
    u8 sector = 0;
    for (u64 pos = 0; pos < 0xC00; pos += 0x200)
    {
        aes128XtsContextResetSector(&ctx, sector++, true);
        aes128XtsEncrypt(&ctx, (u8*)&h + pos, (const u8*)&h + pos, 0x200);
    }
    buf.Seek(0);
    buf.Write(&h, sizeof(h));
}

struct Nca
{
    std::vector<u8> data;
    u8 type;
    u8 hash[SHA256_HASH_SIZE];
    Nca(std::vector<u8> d, u8 t) : data(std::move(d)), type(t) { sha256CalculateHash(hash, data.data(), data.size()); }
};

// Patches the loader's NPDM for this entry: its title id, a 39-bit address space (as games have),
// three cores, and (Atmosphere 1.8.0 on) the debug flag sphaira's loader sets.
bool PatchNpdm(std::vector<u8>& npdm, u64 tid)
{
    if (npdm.size() < sizeof(NpdmMeta)) return false;
    NpdmMeta meta;
    memcpy(&meta, npdm.data(), sizeof(meta));
    if (meta.aci0_offset + sizeof(NpdmAci0) > npdm.size() || meta.acid_offset + sizeof(NpdmAcid) > npdm.size()) return false;
    NpdmAci0 aci0;
    NpdmAcid acid;
    memcpy(&aci0, npdm.data() + meta.aci0_offset, sizeof(aci0));
    memcpy(&acid, npdm.data() + meta.acid_offset, sizeof(acid));
    memset(meta.title_name, 0, sizeof(meta.title_name));
    strcpy(meta.title_name, "Application");
    meta.flags = (meta.flags & ~(0x7 << 1)) | (3 << 1); // 39-bit address space
    aci0.program_id = tid;
    acid.program_id_min = tid;
    acid.program_id_max = tid;
    auto patchKc = [&](u32 off, u32 size, u32 bitmask, u32 value) {
        const u32 pattern = BIT(bitmask) - 1, mask = BIT(bitmask) | pattern;
        for (u32 i = 0; i + 4 <= size; i += 4)
        {
            u32 cap;
            memcpy(&cap, npdm.data() + off + i, 4);
            if ((cap & mask) == pattern)
            {
                cap = value | pattern;
                memcpy(npdm.data() + off + i, &cap, 4);
                return true;
            }
        }
        return false;
    };
    // Kernel flags: cores 0 to 2, priorities 28 to 59.
    const u32 kernelFlags = ((((2u << 8) | 0u) << 6 | 28u) << 6 | 59u) << 4;
    const bool ok = patchKc(meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 3, kernelFlags) &&
                    patchKc(meta.acid_offset + acid.kac_offset, acid.kac_size, 3, kernelFlags);
    u64 ver = 0;
    if (R_SUCCEEDED(splInitialize()))
    {
        if (R_SUCCEEDED(splGetConfig((SplConfigItem)65000, &ver)) && (ver >> 40) >= MAKEHOSVERSION(1, 8, 0))
        {
            patchKc(meta.aci0_offset + aci0.kac_offset, aci0.kac_size, 16, BIT(19));
            patchKc(meta.acid_offset + acid.kac_offset, acid.kac_size, 16, BIT(19));
        }
        splExit();
    }
    memcpy(npdm.data(), &meta, sizeof(meta));
    memcpy(npdm.data() + meta.aci0_offset, &aci0, sizeof(aci0));
    memcpy(npdm.data() + meta.acid_offset, &acid, sizeof(acid));
    return ok;
}

void PatchNacp(NacpStruct& nacp, const std::string& name, u64 tid)
{
    for (NacpLanguageEntry& lang : nacp.lang)
    {
        memset(lang.name, 0, sizeof(lang.name));
        strncpy(lang.name, name.c_str(), sizeof(lang.name) - 1);
        memset(lang.author, 0, sizeof(lang.author));
        strncpy(lang.author, "Pokémon Fulldex", sizeof(lang.author) - 1);
    }
    nacp.startup_user_account = 0;           // no user to choose
    nacp.user_account_switch_lock = 0;
    nacp.screenshot = 0;                     // allowed
    nacp.video_capture = 2;                  // automatic
    nacp.logo_type = 2;
    nacp.logo_handling = 0;
    nacp.data_loss_confirmation = 0;         // (no save data of its own)
    nacp.presence_group_id = tid;
    nacp.save_data_owner_id = tid;
    nacp.pseudo_device_id_seed = tid;
    nacp.add_on_content_base_id = tid ^ 0x1000;
    for (u64& id : nacp.local_communication_id) id = tid;
    nacp.user_account_save_data_size = 0;
    nacp.user_account_save_data_journal_size = 0;
    nacp.device_save_data_size = 0;
    nacp.device_save_data_journal_size = 0;
}

bool ReadFile(const std::string& path, std::vector<u8>& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(size > 0 ? (size_t)size : 0);
    const bool ok = size > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

// The NACP inside the .nro (its asset section), the base for each entry's.
bool NroNacp(const std::string& nroPath, NacpStruct& nacp)
{
    FILE* f = fopen(nroPath.c_str(), "rb");
    if (!f) return false;
    NroHeader header;
    NroAssetHeader assets;
    bool ok = fseek(f, sizeof(NroStart), SEEK_SET) == 0 && fread(&header, sizeof(header), 1, f) == 1 && header.magic == NROHEADER_MAGIC &&
              fseek(f, header.size, SEEK_SET) == 0 && fread(&assets, sizeof(assets), 1, f) == 1 && assets.magic == NROASSETHEADER_MAGIC &&
              assets.nacp.size >= sizeof(NacpStruct) && fseek(f, header.size + assets.nacp.offset, SEEK_SET) == 0 &&
              fread(&nacp, sizeof(nacp), 1, f) == 1;
    fclose(f);
    return ok;
}

// The application manager's commands sphaira uses that libnx has no call for.
Result PushApplicationRecord(Service* srv, u64 tid, const ContentStorageRecord* records, u32 count)
{
    const struct { u8 last_modified_event; u8 padding[7]; u64 tid; } in = { 3 /* installed */, { 0 }, tid };
    return serviceDispatchIn(srv, 16, in, .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_In },
                             .buffers = { { records, sizeof(*records) * count } });
}
Result InvalidateApplicationControlCache(Service* srv, u64 tid) { return serviceDispatchIn(srv, 404, tid); }

const char* const kLanguages[] = { "AmericanEnglish", "BritishEnglish", "Japanese", "French", "German", "LatinAmericanSpanish",
                                   "Spanish", "Italian", "Dutch", "CanadianFrench", "Portuguese", "Russian", "Korean",
                                   "TraditionalChinese", "SimplifiedChinese", "BrazilianPortuguese" };

Result InstallOne(const std::string& nroPath, const Entry& e, const std::vector<u8>& hblMain, const std::vector<u8>& hblNpdm,
                  const NacpStruct& baseNacp, const u8* headerKey, NcmStorageId storage, Service* nsApp)
{
    const std::string quoted = nroPath.find(' ') == std::string::npos ? nroPath : "\"" + nroPath + "\"";
    const std::string argv = e.args.empty() ? quoted : quoted + " " + e.args;
    // The title id: a hash of the path and arguments (the same entry made again replaces itself),
    // in the 0x05 range sphaira uses for forwarders.
    u64 hash[SHA256_HASH_SIZE / sizeof(u64)];
    const std::string hashed = "pokemon-ds-wide " + nroPath + " " + argv;
    sha256CalculateHash(hash, hashed.data(), hashed.size());
    const u64 tid = 0x0500000000000000ull | (hash[0] & 0x00FFFFFFFFFFF000ull);

    std::vector<Nca> ncas;
    {
        Files exefs;
        AddFile(exefs, "main", hblMain.data(), hblMain.size());
        AddFile(exefs, "main.npdm", hblNpdm.data(), hblNpdm.size());
        if (!PatchNpdm(exefs[1].data, tid)) return MAKERESULT(Module_Libnx, LibnxError_BadInput);
        Files romfs;
        AddFile(romfs, "/nextArgv", argv.data(), argv.size());
        AddFile(romfs, "/nextNroPath", nroPath.data(), nroPath.size());
        NcaHeader h = {};
        Buf buf;
        buf.Write(&h, sizeof(h));
        WritePfs0Section(h, 0, exefs, PFS0_EXEFS_HASH_BLOCK_SIZE, buf);
        WriteRomfsSection(h, 1, romfs, buf);
        FinishHeader(h, tid, headerKey, ContentType_Program, buf);
        ncas.emplace_back(std::move(buf.buf), NcmContentType_Program);
    }
    {
        NacpStruct nacp = baseNacp;
        PatchNacp(nacp, e.name, tid);
        Files romfs;
        AddFile(romfs, "/control.nacp", &nacp, sizeof(nacp));
        // (The icon for every language, so that the HOME Menu finds one whatever the console's.)
        for (const char* lang : kLanguages)
            AddFile(romfs, (std::string("/icon_") + lang + ".dat").c_str(), e.icon.data(), e.icon.size());
        NcaHeader h = {};
        Buf buf;
        buf.Write(&h, sizeof(h));
        WriteRomfsSection(h, 0, romfs, buf);
        FinishHeader(h, tid, headerKey, ContentType_Control, buf);
        ncas.emplace_back(std::move(buf.buf), NcmContentType_Control);
    }

    // The meta NCA: what the application is made of.
    CnmtHeader cnmt = {};
    NcmApplicationMetaExtendedHeader ext = {};
    NcmPackagedContentInfo packaged[2] = {};
    cnmt.title_id = tid;
    cnmt.meta_type = NcmContentMetaType_Application;
    cnmt.meta_header.extended_header_size = sizeof(ext);
    cnmt.meta_header.content_count = 2;
    cnmt.meta_header.content_meta_count = 1;
    cnmt.meta_header.storage_id = storage;
    ext.patch_id = tid | 0x800;
    for (int i = 0; i < 2; i++)
    {
        memcpy(packaged[i].hash, ncas[i].hash, sizeof(packaged[i].hash));
        memcpy(&packaged[i].info.content_id, ncas[i].hash, sizeof(packaged[i].info.content_id));
        packaged[i].info.content_type = ncas[i].type;
        ncmU64ToContentInfoSize(ncas[i].data.size(), &packaged[i].info);
    }
    Buf cnmtBuf;
    u8 digest[0x20] = {};
    cnmtBuf.Write(&cnmt, sizeof(cnmt));
    cnmtBuf.Write(&ext, sizeof(ext));
    cnmtBuf.Write(packaged, sizeof(packaged));
    cnmtBuf.Write(digest, sizeof(digest));
    {
        Files files;
        char name[40];
        snprintf(name, sizeof(name), "Application_%016lX.cnmt", (unsigned long)tid);
        AddFile(files, name, cnmtBuf.buf.data(), cnmtBuf.buf.size());
        NcaHeader h = {};
        Buf buf;
        buf.Write(&h, sizeof(h));
        WritePfs0Section(h, 0, files, PFS0_META_HASH_BLOCK_SIZE, buf);
        FinishHeader(h, tid, headerKey, ContentType_Meta, buf);
        ncas.emplace_back(std::move(buf.buf), NcmContentType_Meta);
    }

    NcmContentMetaKey key = {};
    key.id = tid;
    key.version = 0;
    key.type = NcmContentMetaType_Application;
    key.install_type = NcmContentInstallType_Full;
    ContentStorageRecord record = {};
    record.key = key;
    record.storage_id = storage;
    ContentMetaData data = {};
    data.header = cnmt.meta_header;
    data.header.content_count++;
    data.header.storage_id = 0;
    data.extended = ext;
    memcpy(&data.infos[0].content_id, ncas[2].hash, sizeof(data.infos[0].content_id));
    data.infos[0].content_type = NcmContentType_Meta;
    ncmU64ToContentInfoSize(cnmtBuf.buf.size(), &data.infos[0]);
    data.infos[1] = packaged[0].info;
    data.infos[2] = packaged[1].info;

    if (!g_debugFolder.empty())
        for (const Nca& nca : ncas)
        {
            char path[512];
            snprintf(path, sizeof path, "%s/%016lX-%d.nca", g_debugFolder.c_str(), (unsigned long)tid, (int)nca.type);
            if (FILE* f = fopen(path, "wb"))
            {
                fwrite(nca.data.data(), 1, nca.data.size(), f);
                fclose(f);
            }
        }
    // An entry made before goes first, record and parts: its parts are the same files as the new
    // ones when nothing has changed, and removing it after writing them took them away again
    // (the HOME Menu then showed the entry loading forever).
    nsDeleteApplicationEntity(tid);
    // Written to the storage, registered, and the HOME Menu told.
    NcmContentStorage cs;
    Result rc = ncmOpenContentStorage(&cs, storage);
    if (R_FAILED(rc)) return rc;
    for (const Nca& nca : ncas)
    {
        NcmContentId id;
        NcmPlaceHolderId ph;
        memcpy(&id, nca.hash, sizeof(id));
        if (R_FAILED(rc = ncmContentStorageGeneratePlaceHolderId(&cs, &ph))) break;
        ncmContentStorageDeletePlaceHolder(&cs, &ph);
        if (R_FAILED(rc = ncmContentStorageCreatePlaceHolder(&cs, &id, &ph, nca.data.size()))) break;
        if (R_FAILED(rc = ncmContentStorageWritePlaceHolder(&cs, &ph, 0, nca.data.data(), nca.data.size()))) break;
        ncmContentStorageDelete(&cs, &id);
        if (R_FAILED(rc = ncmContentStorageRegister(&cs, &id, &ph))) break;
    }
    ncmContentStorageClose(&cs);
    if (R_FAILED(rc)) return rc;

    NcmContentMetaDatabase db;
    if (R_FAILED(rc = ncmOpenContentMetaDatabase(&db, storage))) return rc;
    rc = ncmContentMetaDatabaseSet(&db, &key, &data, sizeof(data));
    if (R_SUCCEEDED(rc)) rc = ncmContentMetaDatabaseCommit(&db);
    ncmContentMetaDatabaseClose(&db);
    if (R_FAILED(rc)) return rc;

    if (R_FAILED(rc = PushApplicationRecord(nsApp, tid, &record, 1))) return rc;
    InvalidateApplicationControlCache(nsApp, tid);
    fprintf(stderr, "[forwarder] %s: title %016lX\n", e.name.c_str(), (unsigned long)tid);
    return 0;
}
}

void SetDebugFolder(const std::string& folder) { g_debugFolder = folder; }

int Install(const std::string& nroPath, const std::vector<Entry>& entries, std::string& error)
{
    std::vector<u8> hblMain, hblNpdm;
    if (!ReadFile("romfs:/forwarder/main", hblMain) || !ReadFile("romfs:/forwarder/main.npdm", hblNpdm))
    {
        error = "THIS BUILD HAS NO FORWARDER";
        return 0;
    }
    NacpStruct nacp;
    if (!NroNacp(nroPath, nacp))
    {
        error = "CANNOT READ " + nroPath.substr(nroPath.find_last_of('/') + 1);
        return 0;
    }
    // The NCA header key, made by the console from its sources.
    u8 headerKey[0x20];
    Result rc = splCryptoInitialize();
    if (R_SUCCEEDED(rc))
    {
        u8 kek[0x20];
        rc = splCryptoGenerateAesKek(HEADER_KEK_SRC, 0, 0, kek);
        if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(kek, HEADER_KEY_SRC, headerKey);
        if (R_SUCCEEDED(rc)) rc = splCryptoGenerateAesKey(kek, HEADER_KEY_SRC + 0x10, headerKey + 0x10);
        splCryptoExit();
    }
    if (R_FAILED(rc))
    {
        char msg[64];
        snprintf(msg, sizeof msg, "NO KEY (0x%X)", (unsigned)rc);
        error = msg;
        return 0;
    }
    if (R_FAILED(rc = ncmInitialize()))
    {
        error = "NO NCM";
        return 0;
    }
    Service nsApp = {};
    if (R_FAILED(rc = nsInitialize()) || R_FAILED(rc = nsGetApplicationManagerInterface(&nsApp)))
    {
        ncmExit();
        error = "NO NS";
        return 0;
    }
    int made = 0;
    for (const Entry& e : entries)
    {
        // The SD card, or the console's own storage if the SD card cannot take it.
        rc = InstallOne(nroPath, e, hblMain, hblNpdm, nacp, headerKey, NcmStorageId_SdCard, &nsApp);
        if (R_FAILED(rc)) rc = InstallOne(nroPath, e, hblMain, hblNpdm, nacp, headerKey, NcmStorageId_BuiltInUser, &nsApp);
        if (R_SUCCEEDED(rc)) made++;
        else
        {
            char msg[64];
            snprintf(msg, sizeof msg, "ERROR 0x%X", (unsigned)rc);
            error = msg;
            fprintf(stderr, "[forwarder] %s: failed, 0x%x\n", e.name.c_str(), (unsigned)rc);
        }
    }
    serviceClose(&nsApp);
    nsExit();
    ncmExit();
    return made;
}
}
