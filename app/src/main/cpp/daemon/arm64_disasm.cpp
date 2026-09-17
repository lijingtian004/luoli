#include "arm64_disasm.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace eng {

static inline uint32_t extract(uint32_t val, int hi, int lo) {
    return (val >> lo) & ((1U << (hi - lo + 1)) - 1);
}

static inline int32_t signExtend(uint32_t val, int bits) {
    uint32_t m = 1U << (bits - 1);
    return (int32_t)((val ^ m) - m);
}

static inline int64_t signExtend64(uint64_t val, int bits) {
    uint64_t m = 1ULL << (bits - 1);
    return (int64_t)((val ^ m) - m);
}

static const char *condName(uint32_t cond) {
    static const char *names[] = {
        "eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
        "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"
    };
    return (cond < 16) ? names[cond] : "??";
}

static std::string regName(int idx, bool is64, bool isSp = false) {
    if (idx == 31) return isSp ? (is64 ? "sp" : "wsp") : (is64 ? "xzr" : "wzr");
    char b[8];
    snprintf(b, sizeof(b), "%c%d", is64 ? 'x' : 'w', idx);
    return b;
}

static std::string vregName(int idx, char prefix = 's') {
    char b[8];
    snprintf(b, sizeof(b), "%c%d", prefix, idx);
    return b;
}

void decodeArm64(uint32_t insn, uint64_t pc, std::string &mnemonic, std::string &operands) {
    if (insn == 0xD503201F) { mnemonic = "nop"; operands = ""; return; }
    if (insn == 0xD65F03C0) { mnemonic = "ret"; operands = ""; return; }

    char buf[128];

    // Branch to Register: br, blr, ret
    // 1101011 0000/0001/0010 11111 000000 Rn 00000
    if ((insn & 0xFFFFFC1F) == 0xD61F0000) {
        int op = extract(insn, 22, 21);
        int rn = extract(insn, 9, 5);
        if (op == 0) mnemonic = "br";
        else if (op == 1) mnemonic = "blr";
        else if (op == 2) mnemonic = "ret";
        else mnemonic = ".inst";
        operands = (op == 2 && rn == 30) ? "" : regName(rn, true);
        return;
    }

    // Branch (immediate): b, bl
    // op 00101 imm26
    if ((insn & 0x7C000000) == 0x14000000) {
        bool link = (insn & 0x80000000) != 0;
        mnemonic = link ? "bl" : "b";
        int64_t off = signExtend64((uint64_t)extract(insn, 25, 0) << 2, 28);
        uint64_t target = pc + off;
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)target);
        operands = buf;
        return;
    }

    // Conditional Branch: b.cond
    // 01010100 imm19 0 cond
    if ((insn & 0xFF000010) == 0x54000000) {
        uint32_t cond = extract(insn, 3, 0);
        mnemonic = std::string("b.") + condName(cond);
        int64_t off = signExtend64((uint64_t)extract(insn, 23, 5) << 2, 21);
        uint64_t target = pc + off;
        snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)target);
        operands = buf;
        return;
    }

    // Compare and Branch: cbz, cbnz
    // sf 011010 op imm19 Rt
    if ((insn & 0x7E000000) == 0x34000000) {
        bool sf = (insn & 0x80000000) != 0;
        bool op = (insn & 0x01000000) != 0;
        int rt = extract(insn, 4, 0);
        int64_t off = signExtend64((uint64_t)extract(insn, 23, 5) << 2, 21);
        mnemonic = op ? "cbnz" : "cbz";
        snprintf(buf, sizeof(buf), "%s, 0x%llx", regName(rt, sf).c_str(), (unsigned long long)(pc + off));
        operands = buf;
        return;
    }

    // Test and Branch: tbz, tbnz
    // b5 011011 op b40 imm14 Rt
    if ((insn & 0x7E000000) == 0x36000000) {
        bool op = (insn & 0x01000000) != 0;
        int bit = (extract(insn, 31, 31) << 5) | extract(insn, 23, 19);
        int rt = extract(insn, 4, 0);
        int64_t off = signExtend64((uint64_t)extract(insn, 18, 5) << 2, 16);
        mnemonic = op ? "tbnz" : "tbz";
        snprintf(buf, sizeof(buf), "%s, #%d, 0x%llx", regName(rt, true).c_str(), bit, (unsigned long long)(pc + off));
        operands = buf;
        return;
    }

    // PC-rel addressing: adr, adrp
    // op 00 10000
    if ((insn & 0x1F000000) == 0x10000000) {
        bool isAdrp = (insn & 0x80000000) != 0;
        mnemonic = isAdrp ? "adrp" : "adr";
        int rd = extract(insn, 4, 0);
        uint32_t immlo = extract(insn, 30, 29);
        uint32_t immhi = extract(insn, 23, 5);
        int64_t imm = signExtend64(((uint64_t)immhi << 2) | immlo, 21);
        uint64_t target = isAdrp ? ((pc & ~0xFFFULL) + (imm << 12)) : (pc + imm);
        snprintf(buf, sizeof(buf), "%s, 0x%llx", regName(rd, true).c_str(), (unsigned long long)target);
        operands = buf;
        return;
    }

    // Add/Sub (immediate)
    // sf op S 10001 shift imm12 Rn Rd
    if ((insn & 0x1F000000) == 0x11000000) {
        bool sf = (insn & 0x80000000) != 0;
        bool op = (insn & 0x40000000) != 0;
        bool s = (insn & 0x20000000) != 0;
        int shift = extract(insn, 23, 22);
        uint32_t imm12 = extract(insn, 21, 10);
        int rn = extract(insn, 9, 5);
        int rd = extract(insn, 4, 0);
        uint32_t val = shift ? (imm12 << 12) : imm12;

        if (s && rd == 31) {
            mnemonic = op ? "cmp" : "cmn";
            snprintf(buf, sizeof(buf), "%s, #%u", regName(rn, sf, true).c_str(), val);
        } else if (!op && !s && rn == 31 && !shift) {
            mnemonic = "mov";
            snprintf(buf, sizeof(buf), "%s, #%u", regName(rd, sf, true).c_str(), val);
        } else {
            mnemonic = op ? (s ? "subs" : "sub") : (s ? "adds" : "add");
            snprintf(buf, sizeof(buf), "%s, %s, #%u", regName(rd, sf, true).c_str(),
                     regName(rn, sf, true).c_str(), val);
        }
        operands = buf;
        return;
    }

    // Move Wide: movz, movn, movk
    // sf 10 100101 hw imm16 Rd
    if ((insn & 0x1F800000) == 0x12800000) {
        bool sf = (insn & 0x80000000) != 0;
        int opc = extract(insn, 30, 29);
        int hw = extract(insn, 22, 21);
        uint32_t imm16 = extract(insn, 20, 5);
        int rd = extract(insn, 4, 0);
        if (opc == 0) mnemonic = "movn";
        else if (opc == 2) mnemonic = "movz";
        else if (opc == 3) mnemonic = "movk";
        else mnemonic = ".inst";
        if (hw != 0) snprintf(buf, sizeof(buf), "%s, #0x%x, lsl #%d", regName(rd, sf).c_str(), imm16, hw * 16);
        else snprintf(buf, sizeof(buf), "%s, #0x%x", regName(rd, sf).c_str(), imm16);
        operands = buf;
        return;
    }

    // Add/Sub (shifted register)
    // sf op S 01011 shift 0 Rm imm6 Rn Rd
    if ((insn & 0x1F200000) == 0x0B000000) {
        bool sf = (insn & 0x80000000) != 0;
        bool op = (insn & 0x40000000) != 0;
        bool s = (insn & 0x20000000) != 0;
        int rm = extract(insn, 20, 16);
        int imm6 = extract(insn, 15, 10);
        int rn = extract(insn, 9, 5);
        int rd = extract(insn, 4, 0);

        if (s && rd == 31) {
            mnemonic = op ? "cmp" : "cmn";
            if (imm6 == 0) snprintf(buf, sizeof(buf), "%s, %s", regName(rn, sf).c_str(), regName(rm, sf).c_str());
            else snprintf(buf, sizeof(buf), "%s, %s, lsl #%d", regName(rn, sf).c_str(), regName(rm, sf).c_str(), imm6);
        } else if (op && !s && rn == 31) {
            mnemonic = "neg";
            snprintf(buf, sizeof(buf), "%s, %s", regName(rd, sf).c_str(), regName(rm, sf).c_str());
        } else {
            mnemonic = op ? (s ? "subs" : "sub") : (s ? "adds" : "add");
            if (imm6 == 0) {
                snprintf(buf, sizeof(buf), "%s, %s, %s", regName(rd, sf).c_str(), regName(rn, sf).c_str(), regName(rm, sf).c_str());
            } else {
                snprintf(buf, sizeof(buf), "%s, %s, %s, lsl #%d", regName(rd, sf).c_str(), regName(rn, sf).c_str(), regName(rm, sf).c_str(), imm6);
            }
        }
        operands = buf;
        return;
    }

    // Logical (shifted register)
    // sf opc 01010 shift N Rm imm6 Rn Rd
    if ((insn & 0x1F000000) == 0x0A000000) {
        bool sf = (insn & 0x80000000) != 0;
        int opc = extract(insn, 30, 29);
        bool n = (insn & 0x00200000) != 0;
        int rm = extract(insn, 20, 16);
        int rn = extract(insn, 9, 5);
        int rd = extract(insn, 4, 0);

        if (opc == 1 && !n && rn == 31) {
            mnemonic = "mov";
            snprintf(buf, sizeof(buf), "%s, %s", regName(rd, sf).c_str(), regName(rm, sf).c_str());
            operands = buf;
            return;
        } else if (opc == 1 && n && rn == 31) {
            mnemonic = "mvn";
            snprintf(buf, sizeof(buf), "%s, %s", regName(rd, sf).c_str(), regName(rm, sf).c_str());
            operands = buf;
            return;
        }

        if (opc == 0) mnemonic = n ? "bic" : "and";
        else if (opc == 1) mnemonic = n ? "orn" : "orr";
        else if (opc == 2) mnemonic = n ? "eon" : "eor";
        else mnemonic = n ? "bics" : "ands";
        snprintf(buf, sizeof(buf), "%s, %s, %s", regName(rd, sf).c_str(), regName(rn, sf).c_str(), regName(rm, sf).c_str());
        operands = buf;
        return;
    }

    // Load/Store single register (unsigned immediate offset)
    // size 111 0 01 01 opc imm12 Rn Rt
    if ((insn & 0x3B200C00) == 0x39000000) {
        int size = extract(insn, 31, 30);
        int opc = extract(insn, 23, 22);
        int imm12 = extract(insn, 21, 10);
        int rn = extract(insn, 9, 5);
        int rt = extract(insn, 4, 0);

        int scale = size;
        int off = imm12 << scale;
        bool isLoad = (opc & 1) != 0;

        if (size == 0) mnemonic = isLoad ? "ldrb" : "strb";
        else if (size == 1) mnemonic = isLoad ? "ldrh" : "strh";
        else if (size == 2) mnemonic = isLoad ? "ldr" : "str";
        else mnemonic = isLoad ? "ldr" : "str";

        bool is64 = (size == 3);
        if (off != 0) snprintf(buf, sizeof(buf), "%s, [%s, #%d]", regName(rt, is64).c_str(), regName(rn, true, true).c_str(), off);
        else snprintf(buf, sizeof(buf), "%s, [%s]", regName(rt, is64).c_str(), regName(rn, true, true).c_str());
        operands = buf;
        return;
    }

    // Load/Store Pair (LDP / STP)
    // opc 101 0 010 L imm7 Rn Rt2 Rt
    if ((insn & 0x7E400000) == 0x28000000 || (insn & 0x7E400000) == 0x29000000) {
        int opc = extract(insn, 31, 30);
        bool l = (insn & 0x00400000) != 0;
        int imm7 = extract(insn, 21, 15);
        int rt2 = extract(insn, 14, 10);
        int rn = extract(insn, 9, 5);
        int rt = extract(insn, 4, 0);

        bool is64 = (opc == 2);
        int scale = is64 ? 3 : 2;
        int off = signExtend(imm7, 7) << scale;
        mnemonic = l ? "ldp" : "stp";

        if (off != 0) snprintf(buf, sizeof(buf), "%s, %s, [%s, #%d]", regName(rt, is64).c_str(), regName(rt2, is64).c_str(), regName(rn, true, true).c_str(), off);
        else snprintf(buf, sizeof(buf), "%s, %s, [%s]", regName(rt, is64).c_str(), regName(rt2, is64).c_str(), regName(rn, true, true).c_str());
        operands = buf;
        return;
    }

    // Service Call: svc, brk
    if ((insn & 0xFFE0001F) == 0xD4000001) {
        mnemonic = "svc";
        snprintf(buf, sizeof(buf), "#0x%x", extract(insn, 20, 5));
        operands = buf;
        return;
    }
    if ((insn & 0xFFE0001F) == 0xD4200000) {
        mnemonic = "brk";
        snprintf(buf, sizeof(buf), "#0x%x", extract(insn, 20, 5));
        operands = buf;
        return;
    }

    // Fallback: raw hex instruction
    snprintf(buf, sizeof(buf), ".inst 0x%08x", insn);
    mnemonic = buf;
    operands = "";
}

} // namespace eng
