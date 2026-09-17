// 公共定义：类型系统、协议常量
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

namespace eng {

inline constexpr const char *kDaemonVersion = "0.1.0";
inline constexpr const char *kDefaultSocketName = "twt_svc_2778";
inline constexpr const char *kDefaultProcName = "twt_svc";
inline constexpr uint32_t kMaxFrameBytes = 16u * 1024u * 1024u;

enum class VT : uint8_t {
    I8 = 0, U8, I16, U16, I32, U32, I64, U64, F32, F64,
};

inline size_t vtSize(VT t) {
    switch (t) {
        case VT::I8: case VT::U8: return 1;
        case VT::I16: case VT::U16: return 2;
        case VT::I32: case VT::U32: case VT::F32: return 4;
        case VT::I64: case VT::U64: case VT::F64: return 8;
    }
    return 0;
}

// "i32"/"f64" -> VT；失败返回 false
inline bool vtFromString(const std::string &s, VT &out) {
    if (s == "i8") out = VT::I8;
    else if (s == "u8") out = VT::U8;
    else if (s == "i16") out = VT::I16;
    else if (s == "u16") out = VT::U16;
    else if (s == "i32") out = VT::I32;
    else if (s == "u32") out = VT::U32;
    else if (s == "i64") out = VT::I64;
    else if (s == "u64") out = VT::U64;
    else if (s == "f32") out = VT::F32;
    else if (s == "f64") out = VT::F64;
    else return false;
    return true;
}

inline const char *vtToString(VT t) {
    switch (t) {
        case VT::I8: return "i8";  case VT::U8: return "u8";
        case VT::I16: return "i16"; case VT::U16: return "u16";
        case VT::I32: return "i32"; case VT::U32: return "u32";
        case VT::I64: return "i64"; case VT::U64: return "u64";
        case VT::F32: return "f32"; case VT::F64: return "f64";
    }
    return "?";
}

// 扫描命中的原始位模式（与地址绑定，类型在扫描时已知）
struct Hit {
    uint64_t addr;
    VT type;
    uint64_t raw;   // 小端位模式，浮点按位存
};

inline double bitsToDouble(VT t, uint64_t raw) {
    switch (t) {
        case VT::F32: { float f; uint32_t b = (uint32_t)raw; memcpy(&f, &b, 4); return (double)f; }
        case VT::F64: { double d; memcpy(&d, &raw, 8); return d; }
        case VT::I8:  return (double)(int8_t)raw;
        case VT::U8:  return (double)(uint8_t)raw;
        case VT::I16: return (double)(int16_t)raw;
        case VT::U16: return (double)(uint16_t)raw;
        case VT::I32: return (double)(int32_t)raw;
        case VT::U32: return (double)(uint32_t)raw;
        case VT::I64: return (double)(int64_t)raw;
        case VT::U64: return (double)raw;
    }
    return 0.0;
}

inline uint64_t doubleToBits(VT t, double v) {
    switch (t) {
        case VT::F32: { float f = (float)v; uint32_t b; memcpy(&b, &f, 4); return b; }
        case VT::F64: { uint64_t b; memcpy(&b, &v, 8); return b; }
        case VT::I8:  return (uint64_t)(uint8_t)(int8_t)v;
        case VT::U8:  return (uint64_t)(uint8_t)v;
        case VT::I16: return (uint64_t)(uint16_t)(int16_t)v;
        case VT::U16: return (uint64_t)(uint16_t)v;
        case VT::I32: return (uint64_t)(uint32_t)(int32_t)v;
        case VT::U32: return (uint64_t)(uint32_t)v;
        case VT::I64: return (uint64_t)(int64_t)v;
        case VT::U64: return (uint64_t)v;
    }
    return 0;
}

inline int cmpValues(VT t, uint64_t a, uint64_t b) {
    switch (t) {
        case VT::I8: { int8_t x = (int8_t)a, y = (int8_t)b; return x < y ? -1 : x > y ? 1 : 0; }
        case VT::I16: { int16_t x = (int16_t)a, y = (int16_t)b; return x < y ? -1 : x > y ? 1 : 0; }
        case VT::I32: { int32_t x = (int32_t)a, y = (int32_t)b; return x < y ? -1 : x > y ? 1 : 0; }
        case VT::I64: { int64_t x = (int64_t)a, y = (int64_t)b; return x < y ? -1 : x > y ? 1 : 0; }
        case VT::F32: { float x, y; uint32_t u32a = (uint32_t)a, u32b = (uint32_t)b; memcpy(&x, &u32a, 4); memcpy(&y, &u32b, 4); return x < y ? -1 : x > y ? 1 : 0; }
        case VT::F64: { double x, y; memcpy(&x, &a, 8); memcpy(&y, &b, 8); return x < y ? -1 : x > y ? 1 : 0; }
        default: return a < b ? -1 : a > b ? 1 : 0;
    }
}

} // namespace eng
