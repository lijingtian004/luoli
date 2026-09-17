#pragma once

#include <cstdint>
#include <string>

namespace eng {

// 解码一条 32 位 AArch64 指令，返回助记符与操作数
void decodeArm64(uint32_t insn, uint64_t pc, std::string &mnemonic, std::string &operands);

} // namespace eng
