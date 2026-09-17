#include "il2cpp.h"

#include <elf.h>
#include <cstring>
#include <algorithm>

namespace eng {

Il2CppInspector::Il2CppInspector(Scanner &scanner) : scanner_(scanner) {}

bool Il2CppInspector::getStatus(pid_t pid, Il2CppStatus &out) const {
    out = Il2CppStatus();
    std::vector<Scanner::Module> modules;
    if (!Scanner::listModules(pid, modules)) return false;

    for (const auto &m : modules) {
        if (m.name.find("libil2cpp.so") != std::string::npos) {
            out.detected = true;
            out.moduleBase = m.base;
            out.moduleEnd = m.end;
            out.moduleSize = (size_t)(m.end - m.base);
            out.modulePath = m.name;
            break;
        }
    }

    std::vector<Region> regions;
    if (Scanner::listRegions(pid, false, regions)) {
        for (const auto &rg : regions) {
            if (rg.path.find("global-metadata.dat") != std::string::npos) {
                out.hasMetadata = true;
                out.metadataAddr = rg.start;
                break;
            }
        }
    }

    if (!out.hasMetadata) {
        // 兜底扫描内存中解密/映射的 global-metadata (0xFAB11BAF -> AF 1B B1 FA)
        std::vector<Hit> hits;
        std::mutex mu;
        uint64_t total = 0;
        bool truncated = false;
        if (scanner_.searchPattern(pid, "AF 1B B1 FA", 4, "", {"A", "Ca", "Cd"}, 5, hits, mu, total, truncated) && !hits.empty()) {
            out.hasMetadata = true;
            out.metadataAddr = hits[0].addr;
        }
    }

    if (out.hasMetadata && out.metadataAddr != 0) {
        uint64_t w = 0;
        if (scanner_.singleRead(pid, out.metadataAddr, VT::U64, w)) {
            uint32_t magic = (uint32_t)w;
            if (magic == 0xFAB11BAF) {
                out.metadataVersion = (int32_t)(w >> 32);
            }
        }
    }

    if (out.detected) {
        std::vector<Il2CppApiInfo> apis;
        if (listApis(pid, apis)) {
            out.apiCount = apis.size();
        }
    }

    return out.detected;
}

bool Il2CppInspector::listApis(pid_t pid, std::vector<Il2CppApiInfo> &out) const {
    out.clear();
    std::vector<Scanner::Module> modules;
    if (!Scanner::listModules(pid, modules)) return false;

    uint64_t base = 0;
    for (const auto &m : modules) {
        if (m.name.find("libil2cpp.so") != std::string::npos) {
            base = m.base;
            break;
        }
    }
    if (base == 0) return false;

    // 读取 ELF Header
    Elf64_Ehdr ehdr = {};
    uint64_t dummy = 0;
    for (size_t i = 0; i < sizeof(ehdr); i += 8) {
        scanner_.singleRead(pid, base + i, VT::U64, *(uint64_t *)((uint8_t *)&ehdr + i));
    }
    if (memcmp(ehdr.e_ident, ELFMAG, SELFMAG) != 0 || ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        return false;
    }

    // 寻找 PT_DYNAMIC
    uint64_t dynVaddr = 0;
    size_t dynSize = 0;
    for (uint16_t i = 0; i < ehdr.e_phnum; ++i) {
        Elf64_Phdr phdr = {};
        for (size_t k = 0; k < sizeof(phdr); k += 8) {
            scanner_.singleRead(pid, base + ehdr.e_phoff + i * ehdr.e_phentsize + k, VT::U64,
                                *(uint64_t *)((uint8_t *)&phdr + k));
        }
        if (phdr.p_type == PT_DYNAMIC) {
            dynVaddr = base + phdr.p_vaddr;
            dynSize = (size_t)phdr.p_memsz;
            break;
        }
    }
    if (dynVaddr == 0 || dynSize == 0) return false;

    uint64_t symtabAddr = 0;
    uint64_t strtabAddr = 0;
    size_t strtabSize = 0;
    size_t dynCount = dynSize / sizeof(Elf64_Dyn);

    for (size_t i = 0; i < dynCount; ++i) {
        Elf64_Dyn dyn = {};
        for (size_t k = 0; k < sizeof(dyn); k += 8) {
            scanner_.singleRead(pid, dynVaddr + i * sizeof(Elf64_Dyn) + k, VT::U64,
                                *(uint64_t *)((uint8_t *)&dyn + k));
        }
        if (dyn.d_tag == DT_NULL) break;
        if (dyn.d_tag == DT_SYMTAB) symtabAddr = (dyn.d_un.d_ptr < base) ? (base + dyn.d_un.d_ptr) : dyn.d_un.d_ptr;
        if (dyn.d_tag == DT_STRTAB) strtabAddr = (dyn.d_un.d_ptr < base) ? (base + dyn.d_un.d_ptr) : dyn.d_un.d_ptr;
        if (dyn.d_tag == DT_STRSZ) strtabSize = (size_t)dyn.d_un.d_val;
    }

    if (symtabAddr == 0 || strtabAddr == 0) return false;
    if (strtabSize == 0 || strtabSize > 4 * 1024 * 1024) strtabSize = 512 * 1024;

    std::vector<uint8_t> strtab(strtabSize, 0);
    for (size_t pos = 0; pos < strtabSize; pos += 8) {
        uint64_t val = 0;
        if (scanner_.singleRead(pid, strtabAddr + pos, VT::U64, val)) {
            memcpy(strtab.data() + pos, &val, std::min<size_t>(8, strtabSize - pos));
        } else {
            break;
        }
    }

    // 顺序遍历符号表 (上限 4000 个符号)
    for (size_t idx = 0; idx < 4000; ++idx) {
        Elf64_Sym sym = {};
        for (size_t k = 0; k < sizeof(sym); k += 8) {
            scanner_.singleRead(pid, symtabAddr + idx * sizeof(Elf64_Sym) + k, VT::U64,
                                *(uint64_t *)((uint8_t *)&sym + k));
        }
        if (sym.st_name == 0 && sym.st_value == 0 && idx > 10) break;
        if (sym.st_name < strtabSize) {
            const char *name = (const char *)strtab.data() + sym.st_name;
            if (strncmp(name, "il2cpp_", 7) == 0) {
                uint64_t funcAddr = (sym.st_value < base) ? (base + sym.st_value) : sym.st_value;
                out.push_back(Il2CppApiInfo{name, funcAddr});
            }
        }
    }

    return !out.empty();
}

#pragma pack(push, 4)
struct MetadataHeaderCommon {
    uint32_t magic;
    int32_t version;
    int32_t stringLiteralOffset;
    int32_t stringLiteralCount;
    int32_t stringLiteralDataOffset;
    int32_t stringLiteralDataCount;
    int32_t stringOffset;
    int32_t stringCount;
    int32_t eventsOffset;
    int32_t eventsCount;
    int32_t propertiesOffset;
    int32_t propertiesCount;
    int32_t methodsOffset;
    int32_t methodsCount;
    int32_t parameterDefaultValuesOffset;
    int32_t parameterDefaultValuesCount;
    int32_t fieldDefaultValuesOffset;
    int32_t fieldDefaultValuesCount;
    int32_t fieldAndParameterDefaultValueDataOffset;
    int32_t fieldAndParameterDefaultValueDataCount;
    int32_t fieldMarshaledSizesOffset;
    int32_t fieldMarshaledSizesCount;
    int32_t parametersOffset;
    int32_t parametersCount;
    int32_t fieldsOffset;
    int32_t fieldsCount;
    int32_t genericParametersOffset;
    int32_t genericParametersCount;
    int32_t genericParameterConstraintsOffset;
    int32_t genericParameterConstraintsCount;
    int32_t genericContainersOffset;
    int32_t genericContainersCount;
    int32_t nestedTypesOffset;
    int32_t nestedTypesCount;
    int32_t interfacesOffset;
    int32_t interfacesCount;
    int32_t vtableMethodsOffset;
    int32_t vtableMethodsCount;
    int32_t interfaceOffsetsOffset;
    int32_t interfaceOffsetsCount;
    int32_t typeDefinitionsOffset;
    int32_t typeDefinitionsCount;
};
#pragma pack(pop)

bool Il2CppInspector::inspectClass(pid_t pid, const std::string &className, std::vector<Il2CppClassInfo> &out) const {
    out.clear();
    uint64_t metadataAddr = 0;

    std::vector<Region> regions;
    if (Scanner::listRegions(pid, false, regions)) {
        for (const auto &rg : regions) {
            if (rg.path.find("global-metadata.dat") != std::string::npos) {
                metadataAddr = rg.start;
                break;
            }
        }
    }

    if (metadataAddr == 0) {
        // 兜底扫描各段起始 magic
        for (const auto &rg : regions) {
            if (rg.end - rg.start >= sizeof(MetadataHeaderCommon)) {
                uint32_t magic = 0;
                if (scanner_.singleRead(pid, rg.start, VT::U32, *(uint64_t *)(void *)&magic) && magic == 0xFAB11BAF) {
                    metadataAddr = rg.start;
                    break;
                }
            }
        }
    }

    if (metadataAddr == 0) {
        // 特征码搜索内存中动态解密或映射的 metadata (0xFAB11BAF)
        std::vector<Hit> hits;
        std::mutex mu;
        uint64_t total = 0;
        bool truncated = false;
        if (scanner_.searchPattern(pid, "AF 1B B1 FA", 4, "", {"A", "Ca", "Cd"}, 5, hits, mu, total, truncated) && !hits.empty()) {
            metadataAddr = hits[0].addr;
        }
    }

    if (metadataAddr == 0) return false;

    MetadataHeaderCommon header = {};
    for (size_t i = 0; i < sizeof(header); i += 8) {
        scanner_.singleRead(pid, metadataAddr + i, VT::U64, *(uint64_t *)((uint8_t *)&header + i));
    }
    if (header.magic != 0xFAB11BAF) return false;

    uint64_t strTabAddr = metadataAddr + header.stringOffset;
    uint64_t typeDefAddr = metadataAddr + header.typeDefinitionsOffset;
    uint64_t fieldsAddr = metadataAddr + header.fieldsOffset;
    int32_t typeCount = std::min<int32_t>(header.typeDefinitionsCount, 30000);

    auto readMetadataStr = [&](uint32_t idx) -> std::string {
        if (idx >= (uint32_t)header.stringCount) return "";
        char sbuf[128] = {};
        for (size_t i = 0; i < sizeof(sbuf) - 1; i += 8) {
            uint64_t w = 0;
            if (scanner_.singleRead(pid, strTabAddr + idx + i, VT::U64, w)) {
                memcpy(sbuf + i, &w, 8);
            }
        }
        sbuf[sizeof(sbuf) - 1] = '\0';
        return std::string(sbuf);
    };

    // 多版本与自适应结构步长校准 (v24: 96B, v27+: 88~92B, v29+ FieldDef: 8B vs 12B)
    int typeDefStride = 92;
    int fieldStartOff = 36;
    int fieldCountOff = 72;
    int tokenOff = 88;
    int fieldDefStride = (header.version >= 29) ? 8 : 12;

    if (header.version < 27) {
        typeDefStride = 96;
        fieldStartOff = 44;
        fieldCountOff = 80;
        tokenOff = 96;
        fieldDefStride = 12;
    }

    // 启发式自校验：通过检查首批 TypeDefinition 的 nameIndex 验证并动态纠正步长
    const int candTypeStrides[] = { typeDefStride, 88, 92, 96, 100, 104 };
    for (int cand : candTypeStrides) {
        bool valid = true;
        int checkCount = std::min<int>(4, typeCount);
        for (int testIdx = 0; testIdx < checkCount; ++testIdx) {
            uint32_t nIdx = 0;
            if (!scanner_.singleRead(pid, typeDefAddr + (uint64_t)testIdx * cand, VT::U32, *(uint64_t *)(void *)&nIdx) ||
                nIdx >= (uint32_t)header.stringCount) {
                valid = false;
                break;
            }
            std::string s = readMetadataStr(nIdx);
            if (s.empty()) { valid = false; break; }
            for (char c : s) {
                if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) { valid = false; break; }
            }
            if (!valid) break;
        }
        if (valid) {
            typeDefStride = cand;
            if (typeDefStride <= 92) {
                fieldStartOff = 36;
                fieldCountOff = 72;
                tokenOff = 88;
            } else {
                fieldStartOff = 44;
                fieldCountOff = 80;
                tokenOff = 96;
            }
            break;
        }
    }

    // 启发式自校验：对 FieldDef 步长 (8 vs 12) 进行动态校验
    const int candFieldStrides[] = { fieldDefStride, 8, 12 };
    for (int cand : candFieldStrides) {
        bool valid = true;
        int checkCount = std::min<int>(4, header.fieldsCount);
        for (int testIdx = 0; testIdx < checkCount; ++testIdx) {
            uint32_t nIdx = 0;
            if (!scanner_.singleRead(pid, fieldsAddr + (uint64_t)testIdx * cand, VT::U32, *(uint64_t *)(void *)&nIdx) ||
                nIdx >= (uint32_t)header.stringCount) {
                valid = false;
                break;
            }
            std::string s = readMetadataStr(nIdx);
            if (s.empty()) { valid = false; break; }
        }
        if (valid) {
            fieldDefStride = cand;
            break;
        }
    }

    std::string lowerTarget = className;
    std::transform(lowerTarget.begin(), lowerTarget.end(), lowerTarget.begin(), ::tolower);

    // 判断目标架构是 64 位还是 32 位（用于基准对象头大小：64位 16 字节，32位 8 字节）
    int baseHeaderSize = 16;
    Scanner::AppArchitectureSummary archSum;
    if (Scanner::inspectArchitecture(pid, archSum) && archSum.bitness == 32) {
        baseHeaderSize = 8;
    }

    for (int32_t i = 0; i < typeCount; ++i) {
        uint64_t curTdAddr = typeDefAddr + (uint64_t)i * typeDefStride;

        uint32_t nameIndex = 0, namespaceIndex = 0, token = 0;
        int32_t fieldStart = 0;
        uint16_t fieldCount = 0;

        scanner_.singleRead(pid, curTdAddr, VT::U32, *(uint64_t *)(void *)&nameIndex);
        scanner_.singleRead(pid, curTdAddr + 4, VT::U32, *(uint64_t *)(void *)&namespaceIndex);
        scanner_.singleRead(pid, curTdAddr + fieldStartOff, VT::I32, *(uint64_t *)(void *)&fieldStart);
        scanner_.singleRead(pid, curTdAddr + fieldCountOff, VT::U16, *(uint64_t *)(void *)&fieldCount);
        scanner_.singleRead(pid, curTdAddr + tokenOff, VT::U32, *(uint64_t *)(void *)&token);

        std::string name = readMetadataStr(nameIndex);
        if (name.empty()) continue;

        std::string lowerName = name;
        std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(), ::tolower);

        if (lowerName.find(lowerTarget) != std::string::npos) {
            Il2CppClassInfo ci;
            ci.name = name;
            ci.namespaze = readMetadataStr(namespaceIndex);
            ci.token = token;
            ci.ok = true;

            int estOffset = baseHeaderSize;
            for (uint16_t f = 0; f < fieldCount && f < 120; ++f) {
                uint64_t fAddr = fieldsAddr + (uint64_t)(fieldStart + f) * fieldDefStride;
                uint32_t fNameIndex = 0;
                int32_t fTypeIndex = 0;

                scanner_.singleRead(pid, fAddr, VT::U32, *(uint64_t *)(void *)&fNameIndex);
                scanner_.singleRead(pid, fAddr + 4, VT::I32, *(uint64_t *)(void *)&fTypeIndex);

                std::string fname = readMetadataStr(fNameIndex);
                if (!fname.empty()) {
                    Il2CppFieldInfo fi;
                    fi.name = fname;
                    fi.offset = estOffset;
                    fi.type = "field";
                    ci.fields.push_back(fi);
                    estOffset += (baseHeaderSize == 16 ? 8 : 4);
                }
            }

            out.push_back(ci);
            if (out.size() >= 25) break;
        }
    }

    return !out.empty();
}

} // namespace eng
