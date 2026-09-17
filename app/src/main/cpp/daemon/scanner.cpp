#include "scanner.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <memory>

#include "arm64_disasm.h"
#include "driver/twt_driver.h"
#include "logger.h"

namespace eng {

twt::Driver *g_driver = nullptr;

// ---------- /proc/<pid>/maps 解析 ----------

static uint64_t hexField(const char *s, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i]; ++i) {
        char c = s[i];
        uint64_t d;
        if (c >= '0' && c <= '9') d = (uint64_t)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (uint64_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (uint64_t)(c - 'A' + 10);
        else break;
        v = (v << 4) | d;
    }
    return v;
}

bool Scanner::listRegions(pid_t pid, bool writableOnly, std::vector<Region> &out) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // start-end perms offset dev inode path
        uint64_t start = hexField(line, 16);
        const char *dash = strchr(line, '-');
        if (!dash) continue;
        uint64_t end = hexField(dash + 1, 16);
        if (end <= start) continue;

        const char *perms = strchr(line, ' ');
        if (!perms) continue;
        while (*perms == ' ') ++perms;
        bool r = perms[0] == 'r';
        bool w = perms[1] == 'w';
        if (!r || (writableOnly && !w)) continue;

        // path：跳过前 5 个字段 (start-end, perms, offset, dev, inode)
        const char *p = line;
        int fields = 0;
        while (*p && fields < 5) {
            if (*p == ' ') {
                ++fields;
                while (*p == ' ') ++p;
            } else {
                ++p;
            }
        }
        while (*p == ' ') ++p;
        std::string name(p);
        if (!name.empty() && name.back() == '\n') name.pop_back();

        // 排除设备映射与无意义区域
        if (name.rfind("/dev/", 0) == 0) continue;
        if (name == "[vvar]" || name == "[vvar_vdso]" || name == "[vectors]") continue;

        out.push_back(Region{start, end, name, classifyTag(name), w});
    }
    fclose(f);
    return true;
}

bool Scanner::listModules(pid_t pid, std::vector<Module> &out) {
    std::vector<Region> regions;
    if (!listRegions(pid, false, regions)) return false;
    for (auto &rg : regions) {
        if (rg.path.empty()) continue;
        if (!out.empty() && out.back().name == rg.path && out.back().end == rg.start) {
            out.back().end = rg.end;
        } else {
            out.push_back(Module{rg.path, rg.start, rg.end});
        }
    }
    return true;
}

// ---------- 限速（漏桶） ----------

void Scanner::rateWait(uint64_t bytes) {
    if (bytes == 0) return;
    std::lock_guard<std::mutex> lk(rateMu_);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    int64_t minInterval = (int64_t)((double)bytes * 1e9 / (double)rate_.load());
    int64_t due = nextTickNs_.load() + minInterval;
    if (now < due) {
        int64_t sleepNs = due - now;
        struct timespec req;
        req.tv_sec = sleepNs / 1000000000ll;
        req.tv_nsec = sleepNs % 1000000000ll;
        nanosleep(&req, nullptr);
        nextTickNs_.store(due);
    } else {
        nextTickNs_.store(now);
    }
}

// ---------- 读取（连续前缀语义：遇洞停止） ----------

static size_t readContiguous(pid_t pid, uint64_t addr, uint8_t *dst, size_t size) {
    if (!g_driver || !g_driver->ready()) return 0;
    if (g_driver->readMemCompat(pid, addr, dst, size)) return size;
    if (size <= 4096) return 0;

    // 二分：填充起始处连续可读的部分
    size_t got = 0;
    size_t pos = 0;
    while (pos < size) {
        size_t blk = std::min<size_t>(size - pos, 1024 * 1024);
        // 逐 1MB 尝试；失败则 4KB 步进跳过
        if (g_driver->readMemCompat(pid, addr + pos, dst + pos, blk)) {
            pos += blk;
            got = pos;
        } else {
            size_t inner = 0;
            while (inner < blk) {
                size_t sub = std::min<size_t>(blk - inner, 4096);
                if (g_driver->readMemCompat(pid, addr + pos + inner, dst + pos + inner, sub)) {
                    inner += sub;
                    got = pos + inner;
                } else {
                    return got;   // 停在第一个洞
                }
            }
            pos += blk;
        }
    }
    return got;
}

bool Scanner::singleRead(pid_t pid, uint64_t addr, VT type, uint64_t &rawOut) const {
    uint64_t raw = 0;
    if (readContiguous(pid, addr, (uint8_t *)&raw, vtSize(type)) != vtSize(type)) return false;
    rawOut = raw;
    return true;
}

bool Scanner::singleWrite(pid_t pid, uint64_t addr, VT type, uint64_t raw) const {
    if (!g_driver || !g_driver->ready()) return false;
    return g_driver->writeMem(pid, addr, &raw, vtSize(type));
}

// ---------- 初扫 ----------

bool Scanner::search(pid_t pid, const ScanOptions &opt, std::vector<Hit> &hits,
                     std::mutex &hitsMu, uint64_t &totalOut, bool &truncatedOut) {
    if (!g_driver || !g_driver->ready()) {
        LOGE("search: driver not ready");
        return false;
    }

    std::vector<Region> regions;
    if (!listRegions(pid, true, regions)) {
        LOGE("search: cannot read maps for pid " + std::to_string(pid));
        return false;
    }

    // 区域过滤（标签分类与路径子串匹配）
    std::vector<Region> chosen;
    for (auto &rg : regions) {
        if (!opt.tags.empty()) {
            bool tagMatch = false;
            for (const auto &t : opt.tags) {
                if (strcasecmp(t.c_str(), rg.tag.c_str()) == 0) {
                    tagMatch = true;
                    break;
                }
            }
            if (!tagMatch) continue;
        }
        if (opt.nameFilter.empty()) {
            chosen.push_back(rg);
        } else {
            std::string lowerPath = rg.path;
            std::string lowerFilter = opt.nameFilter;
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
            std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
            if (lowerPath.find(lowerFilter) != std::string::npos) chosen.push_back(rg);
        }
    }
    if (chosen.empty()) {
        LOGW("search: no region matched filter '" + opt.nameFilter + "'");
        return false;
    }

    uint64_t totalBytes = 0;
    for (auto &rg : chosen) totalBytes += rg.end - rg.start;
    progress.bytesTotal.store(totalBytes);
    progress.bytesDone.store(0);
    progress.found.store(0);
    progress.running.store(true);

    const size_t vsize = vtSize(opt.type);
    const size_t stride = std::max<size_t>(opt.align ? opt.align : vsize, 1);
    const size_t kChunk = 1024 * 1024;

    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk + 64]);
    if (!buf) {
        progress.running.store(false);
        return false;
    }

    bool truncated = false;
    uint64_t foundTotal = 0;
    uint64_t bufStart = chosen[0].start;   // buf[0] 对应的绝对地址
    size_t bufLen = 0;                     // buf 中有效字节数
    // 绝对地址水位：[regionStart, scannedThrough) 内完整值均已记录（防跨界 carry 重复）
    uint64_t scannedThrough = bufStart;

    auto scanBuffer = [&](size_t limit, uint64_t anchor) {
        // 对齐绝对地址；跳过已记录区间
        size_t off0 = 0;
        if (stride > 1) {
            off0 = (size_t)((stride - (bufStart % stride)) % stride);
        }
        size_t limitAdj = limit >= vsize ? limit - vsize + 1 : 0;
        for (size_t off = off0; off < limitAdj; off += stride) {
            uint64_t addr = bufStart + off;
            if (addr + vsize <= scannedThrough) continue;   // 已记录过
            uint64_t bits = 0;
            memcpy(&bits, buf.get() + off, vsize);
            if (!opt.hasTarget || bits == opt.rawTarget) {
                std::lock_guard<std::mutex> lk(hitsMu);
                hits.push_back(Hit{addr, opt.type, bits});
                ++foundTotal;
                progress.found.store(foundTotal);
                if (foundTotal >= opt.maxResults) { truncated = true; return; }
            }
        }
    };

    auto dropTail = [&]() {
        // 保留最后 vsize-1 字节以捕捉跨界值（对齐由 off0 动态计算，无需步进对齐）
        size_t tail = vsize > 1 ? vsize - 1 : 0;
        if (bufLen > tail) {
            memmove(buf.get(), buf.get() + bufLen - tail, tail);
            bufStart += bufLen - tail;
            bufLen = tail;
        }
    };

    for (size_t ri = 0; ri < chosen.size() && !truncated; ++ri) {
        auto &rg = chosen[ri];
        progress.regionIndex.store((int)ri);
        if (ri > 0) { bufStart = rg.start; bufLen = 0; scannedThrough = bufStart; }

        while (!truncated) {
            if (bufStart + bufLen >= rg.end) {
                if (bufLen >= vsize) {
                    scanBuffer(bufLen, rg.start);
                }
                break;
            }
            uint64_t regionLeft = rg.end - (bufStart + bufLen);

            size_t want = (size_t)std::min<uint64_t>(kChunk - bufLen, regionLeft);
            if (want == 0) {
                scanBuffer(bufLen, rg.start);
                scannedThrough = std::max<uint64_t>(scannedThrough, bufStart + bufLen);
                dropTail();
                if (bufStart + bufLen >= rg.end) break;
                continue;
            }
            if (want < vsize && bufLen < vsize) break;

            size_t got = readContiguous(pid, bufStart + bufLen, buf.get() + bufLen, want);
            if (got == 0) {
                // 当前页不可读：快速跳过未提交虚拟内存空洞（指数步进探测，防单页死锁）
                size_t skip = 4096;
                while (skip * 2 <= regionLeft && skip < 16 * 1024 * 1024) {
                    uint8_t probeByte = 0;
                    if (g_driver->readMemCompat(pid, bufStart + bufLen + skip, &probeByte, 1)) {
                        break;
                    }
                    skip *= 2;
                }
                skip = std::min<size_t>(skip, (size_t)regionLeft);
                bufStart += bufLen + skip;
                bufLen = 0;
                scannedThrough = bufStart;
                progress.bytesDone.fetch_add(skip);
                continue;
            }
            rateWait(got);
            progress.bytesDone.fetch_add(got);
            bufLen += got;
            scanBuffer(bufLen, rg.start);
            scannedThrough = bufStart + bufLen;
            if (got < want) {
                // 提前遇洞：丢弃全部（跨界值损失可接受）
                bufStart += bufLen;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            dropTail();
        }
    }

    progress.running.store(false);
    progress.regionIndex.store(-1);
    totalOut = foundTotal;
    truncatedOut = truncated;

    char msg[160];
    snprintf(msg, sizeof(msg), "scan done: found=%llu truncated=%d",
             (unsigned long long)foundTotal, (int)truncated);
    LOGI(msg);
    return true;
}

// ---------- 增量过滤（批量窗口读） ----------

bool Scanner::filter(pid_t pid, const ScanOptions &opt, FilterOp op,
                     std::vector<Hit> &hits, std::mutex &hitsMu,
                     uint64_t &totalOut) {
    const size_t vsize = vtSize(opt.type);
    constexpr size_t kWindow = 64 * 1024;

    std::vector<Hit> snapshot;
    {
        std::lock_guard<std::mutex> lk(hitsMu);
        snapshot = hits;
    }
    if (snapshot.empty()) { totalOut = 0; return true; }
    std::sort(snapshot.begin(), snapshot.end(),
              [](const Hit &a, const Hit &b) { return a.addr < b.addr; });

    std::vector<Hit> kept;
    kept.reserve(snapshot.size() / 2 + 1);

    std::unique_ptr<uint8_t[]> wbuf(new (std::nothrow) uint8_t[kWindow + 64]);
    if (!wbuf) return false;

    progress.bytesTotal.store(snapshot.size() * vsize);
    progress.bytesDone.store(0);
    progress.found.store(0);
    progress.running.store(true);

    auto check = [&](uint64_t cur, uint64_t prev) -> bool {
        uint64_t target = opt.hasTarget ? opt.rawTarget : prev;
        switch (op) {
            case FilterOp::EQ:        return cur == target;
            case FilterOp::NE:        return cur != target;
            case FilterOp::CHANGED:   return cur != prev;
            case FilterOp::UNCHANGED: return cur == prev;
            case FilterOp::GT:        return cmpValues(opt.type, cur, target) > 0;
            case FilterOp::GE:        return cmpValues(opt.type, cur, target) >= 0;
            case FilterOp::LT:        return cmpValues(opt.type, cur, target) < 0;
            case FilterOp::LE:        return cmpValues(opt.type, cur, target) <= 0;
            case FilterOp::INC:       return cmpValues(opt.type, cur, prev) > 0;
            case FilterOp::DEC:       return cmpValues(opt.type, cur, prev) < 0;
        }
        return false;
    };

    size_t i = 0;
    while (i < snapshot.size() && progress.running.load()) {
        uint64_t winStart = snapshot[i].addr;
        size_t j = i;
        while (j < snapshot.size() && snapshot[j].addr + vsize <= winStart + kWindow) ++j;

        size_t winLen = (size_t)(snapshot[j - 1].addr + vsize - winStart);
        bool winOk = readContiguous(pid, winStart, wbuf.get(), winLen) == winLen;

        for (size_t k = i; k < j; ++k) {
            Hit h = snapshot[k];
            uint64_t cur = 0;
            bool ok;
            if (winOk) {
                memcpy(&cur, wbuf.get() + (h.addr - winStart), vsize);
                ok = true;
            } else {
                ok = singleRead(pid, h.addr, h.type, cur);
            }
            if (!ok) continue;   // 读不到 -> 丢弃

            if (check(cur, h.raw)) {
                h.raw = cur;
                kept.push_back(h);
            }
            progress.bytesDone.fetch_add(vsize);
        }
        progress.found.store(kept.size());
        i = j;
        rateWait(winLen ? winLen : kWindow);
    }

    {
        std::lock_guard<std::mutex> lk(hitsMu);
        hits.swap(kept);
        totalOut = hits.size();
    }
    progress.running.store(false);
    return true;
}

// ---------- 内存临域探查 ----------

bool Scanner::inspectMemory(pid_t pid, uint64_t centerAddr, int countBefore, int countAfter,
                            size_t unitSize, MemoryInspection &out) const {
    if (!g_driver || !g_driver->ready()) return false;
    int b = std::clamp(countBefore, 0, 64);
    int a = std::clamp(countAfter, 0, 128);
    if (unitSize != 1 && unitSize != 2 && unitSize != 4 && unitSize != 8) unitSize = 4;

    uint64_t startAddr = centerAddr - (uint64_t)(b * unitSize);
    size_t totalWords = (size_t)(b + 1 + a);
    size_t totalBytes = totalWords * unitSize;

    std::vector<uint8_t> rawBuf(totalBytes, 0);
    size_t rd = readContiguous(pid, startAddr, rawBuf.data(), totalBytes);

    out.centerAddr = centerAddr;
    out.words.clear();
    out.words.reserve(totalWords);

    std::string summary;
    char lineBuf[256];

    for (size_t i = 0; i < totalWords; ++i) {
        uint64_t curAddr = startAddr + i * unitSize;
        int64_t relOff = (int64_t)(curAddr - centerAddr);

        uint64_t rawVal = 0;
        bool readable = false;
        if (i * unitSize + unitSize <= rd) {
            memcpy(&rawVal, rawBuf.data() + i * unitSize, unitSize);
            readable = true;
        } else {
            VT t = (unitSize == 8) ? VT::U64 : (unitSize == 4) ? VT::U32 : (unitSize == 2) ? VT::U16 : VT::U8;
            readable = singleRead(pid, curAddr, t, rawVal);
        }

        MemoryWord w;
        w.offset = relOff;
        w.addr = curAddr;
        w.raw = rawVal;
        w.isTarget = (curAddr == centerAddr);

        if (readable) {
            w.i32 = (int32_t)rawVal;
            w.u32 = (uint32_t)rawVal;
            memcpy(&w.f32, &w.u32, sizeof(float));
            w.i64 = (int64_t)rawVal;
            memcpy(&w.f64, &rawVal, sizeof(double));

            char hexStr[32];
            if (unitSize == 8) snprintf(hexStr, sizeof(hexStr), "0x%016llx", (unsigned long long)rawVal);
            else if (unitSize == 4) snprintf(hexStr, sizeof(hexStr), "0x%08x", (unsigned int)rawVal);
            else if (unitSize == 2) snprintf(hexStr, sizeof(hexStr), "0x%04x", (unsigned int)rawVal);
            else snprintf(hexStr, sizeof(hexStr), "0x%02x", (unsigned int)rawVal);
            w.hex = hexStr;

            char asciiStr[9] = {};
            for (size_t k = 0; k < unitSize && k < 8; ++k) {
                uint8_t c = (rawVal >> (k * 8)) & 0xFF;
                asciiStr[k] = (c >= 32 && c <= 126) ? (char)c : '.';
            }
            w.ascii = asciiStr;

            snprintf(lineBuf, sizeof(lineBuf), "%+4lld [0x%llx]: %-12s i32=%-10d f32=%-10.4g ascii='%s'%s\n",
                     (long long)relOff, (unsigned long long)curAddr, w.hex.c_str(), w.i32, w.f32, w.ascii.c_str(),
                     w.isTarget ? "  <-- TARGET" : "");
        } else {
            w.hex = "??";
            w.ascii = "..";
            snprintf(lineBuf, sizeof(lineBuf), "%+4lld [0x%llx]: [unmapped / unreadable]%s\n",
                     (long long)relOff, (unsigned long long)curAddr, w.isTarget ? "  <-- TARGET" : "");
        }

        summary += lineBuf;
        out.words.push_back(w);
    }

    out.summary = summary;
    out.ok = true;
    return true;
}

// ---------- 群组联合特征扫描 ----------

bool Scanner::searchGroup(pid_t pid, const GroupScanOptions &opt, std::vector<Hit> &hits,
                          std::mutex &hitsMu, uint64_t &totalOut, bool &truncatedOut) {
    if (!g_driver || !g_driver->ready() || opt.items.empty()) return false;

    std::vector<Region> regions;
    if (!listRegions(pid, true, regions)) return false;

    std::vector<Region> chosen;
    for (auto &rg : regions) {
        if (!opt.tags.empty()) {
            bool tagMatch = false;
            for (const auto &t : opt.tags) {
                if (strcasecmp(t.c_str(), rg.tag.c_str()) == 0) { tagMatch = true; break; }
            }
            if (!tagMatch) continue;
        }
        if (opt.nameFilter.empty()) {
            chosen.push_back(rg);
        } else {
            std::string lowerPath = rg.path;
            std::string lowerFilter = opt.nameFilter;
            std::transform(lowerPath.begin(), lowerPath.end(), lowerPath.begin(), ::tolower);
            std::transform(lowerFilter.begin(), lowerFilter.end(), lowerFilter.begin(), ::tolower);
            if (lowerPath.find(lowerFilter) != std::string::npos) chosen.push_back(rg);
        }
    }
    if (chosen.empty()) return false;

    size_t baseIdx = 0;
    for (size_t i = 0; i < opt.items.size(); ++i) {
        if (opt.items[i].offset == 0) { baseIdx = i; break; }
    }
    const auto &baseItem = opt.items[baseIdx];

    int64_t minOff = 0, maxOff = 0;
    for (const auto &it : opt.items) {
        minOff = std::min(minOff, it.offset);
        maxOff = std::max(maxOff, it.offset);
    }
    size_t span = (size_t)(maxOff - minOff) + 64;

    size_t baseVsize = vtSize(baseItem.type);
    size_t stride = std::max<size_t>(opt.baseAlign ? opt.baseAlign : baseVsize, 1);
    size_t kChunk = 1024 * 1024;

    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk + span + 64]);
    if (!buf) return false;

    uint64_t foundTotal = 0;
    bool truncated = false;
    uint64_t bufStart = chosen[0].start;
    size_t bufLen = 0;
    uint64_t scannedThrough = bufStart;

    auto checkGroupAt = [&](uint64_t baseAddr) -> bool {
        for (const auto &it : opt.items) {
            uint64_t targetAddr = baseAddr + it.offset;
            size_t sz = vtSize(it.type);
            uint64_t bits = 0;
            if (targetAddr >= bufStart && targetAddr + sz <= bufStart + bufLen) {
                memcpy(&bits, buf.get() + (targetAddr - bufStart), sz);
            } else {
                if (!singleRead(pid, targetAddr, it.type, bits)) return false;
            }
            if (bits != it.rawTarget) return false;
        }
        return true;
    };

    auto scanBuffer = [&](size_t limit) {
        size_t off0 = 0;
        if (stride > 1) off0 = (size_t)((stride - (bufStart % stride)) % stride);
        size_t limitAdj = limit >= baseVsize ? limit - baseVsize + 1 : 0;
        for (size_t off = off0; off < limitAdj; off += stride) {
            uint64_t curAddr = bufStart + off;
            uint64_t baseAddr = curAddr - baseItem.offset;
            if (baseAddr + baseVsize <= scannedThrough) continue;
            uint64_t bBits = 0;
            memcpy(&bBits, buf.get() + off, baseVsize);
            if (bBits == baseItem.rawTarget) {
                if (checkGroupAt(baseAddr)) {
                    std::lock_guard<std::mutex> lk(hitsMu);
                    hits.push_back(Hit{baseAddr, baseItem.type, bBits});
                    ++foundTotal;
                    if (foundTotal >= opt.maxResults) { truncated = true; return; }
                }
            }
        }
    };

    auto dropTail = [&]() {
        size_t tail = span > baseVsize ? span : baseVsize;
        if (bufLen > tail) {
            memmove(buf.get(), buf.get() + bufLen - tail, tail);
            bufStart += bufLen - tail;
            bufLen = tail;
        }
    };

    for (size_t ri = 0; ri < chosen.size() && !truncated; ++ri) {
        auto &rg = chosen[ri];
        if (ri > 0) { bufStart = rg.start; bufLen = 0; scannedThrough = bufStart; }

        while (!truncated) {
            if (bufStart + bufLen >= rg.end) {
                if (bufLen >= baseVsize) scanBuffer(bufLen);
                break;
            }
            uint64_t regionLeft = rg.end - (bufStart + bufLen);
            size_t want = (size_t)std::min<uint64_t>(kChunk - bufLen, regionLeft);
            if (want == 0) {
                scanBuffer(bufLen);
                scannedThrough = std::max<uint64_t>(scannedThrough, bufStart + bufLen);
                dropTail();
                if (bufStart + bufLen >= rg.end) break;
                continue;
            }
            if (want < baseVsize && bufLen < baseVsize) break;

            size_t got = readContiguous(pid, bufStart + bufLen, buf.get() + bufLen, want);
            if (got == 0) {
                size_t skip = 4096;
                while (skip * 2 <= regionLeft && skip < 16 * 1024 * 1024) {
                    uint8_t p = 0;
                    if (g_driver->readMemCompat(pid, bufStart + bufLen + skip, &p, 1)) break;
                    skip *= 2;
                }
                skip = std::min<size_t>(skip, (size_t)regionLeft);
                bufStart += bufLen + skip;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            bufLen += got;
            scanBuffer(bufLen);
            scannedThrough = bufStart + bufLen;
            if (got < want) {
                bufStart += bufLen;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            dropTail();
        }
    }

    totalOut = foundTotal;
    truncatedOut = truncated;
    return true;
}

// ---------- 指针扫描器 ----------

bool Scanner::findPointers(pid_t pid, uint64_t targetAddr, int64_t maxOffset, size_t align,
                           const std::vector<std::string> &tags, std::vector<PointerHit> &out) const {
    if (!g_driver || !g_driver->ready() || targetAddr == 0) return false;

    std::vector<Region> regions;
    if (!listRegions(pid, true, regions)) return false;

    std::vector<Module> modules;
    listModules(pid, modules);

    std::vector<Region> chosen;
    for (auto &rg : regions) {
        if (!tags.empty()) {
            bool tagMatch = false;
            for (const auto &t : tags) {
                if (strcasecmp(t.c_str(), rg.tag.c_str()) == 0) { tagMatch = true; break; }
            }
            if (!tagMatch) continue;
        } else {
            // 默认优先扫描静态 BSS、堆与匿名段
            if (rg.tag != "B" && rg.tag != "Jh" && rg.tag != "Ch" && rg.tag != "Cd" && rg.tag != "A") {
                continue;
            }
        }
        chosen.push_back(rg);
    }
    if (chosen.empty()) return false;

    size_t pAlign = (align == 4 || align == 8) ? align : 8;
    size_t kChunk = 1024 * 1024;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk + 16]);
    if (!buf) return false;

    for (auto &rg : chosen) {
        uint64_t pos = rg.start;
        while (pos < rg.end) {
            size_t want = (size_t)std::min<uint64_t>(kChunk, rg.end - pos);
            size_t got = readContiguous(pid, pos, buf.get(), want);
            if (got < 8) {
                pos += want;
                continue;
            }

            size_t off0 = (size_t)((pAlign - (pos % pAlign)) % pAlign);
            for (size_t off = off0; off + pAlign <= got; off += pAlign) {
                uint64_t matchedVal = 0;
                bool matched = false;

                // 优先检查 8 字节 64 位指针
                if (off + 8 <= got) {
                    uint64_t val = 0;
                    memcpy(&val, buf.get() + off, 8);
                    val &= 0xFFFFFFFFFFFFULL;
                    if (val > 0x10000 && val <= targetAddr && targetAddr - val <= (uint64_t)maxOffset) {
                        matchedVal = val;
                        matched = true;
                    }
                }

                // 若 64 位未命中且按 4 字节对齐，检查 32 位引用（如 ART Compressed OOP / 32位堆指针）
                if (!matched && pAlign == 4 && off + 4 <= got) {
                    uint32_t val32 = 0;
                    memcpy(&val32, buf.get() + off, 4);
                    uint64_t val = val32;
                    if (val > 0x10000 && val <= targetAddr && targetAddr - val <= (uint64_t)maxOffset) {
                        matchedVal = val;
                        matched = true;
                    }
                }

                if (matched) {
                    uint64_t pAddr = pos + off;
                    int64_t diff = (int64_t)(targetAddr - matchedVal);

                    std::string modDesc;
                    for (const auto &m : modules) {
                        if (pAddr >= m.base && pAddr < m.end) {
                            char b[64];
                            snprintf(b, sizeof(b), "+0x%llx", (unsigned long long)(pAddr - m.base));
                            size_t slash = m.name.rfind('/');
                            std::string baseName = (slash != std::string::npos) ? m.name.substr(slash + 1) : m.name;
                            modDesc = baseName + b;
                            break;
                        }
                    }

                    out.push_back(PointerHit{pAddr, matchedVal, diff, rg.tag, modDesc});
                    if (out.size() >= 1000) return true; // Cap to 1000 pointers
                }
            }
            pos += got;
        }
    }
    return true;
}

// ---------- 字符串搜索 ----------

bool Scanner::searchString(pid_t pid, const std::string &text, const std::string &encoding,
                           const std::string &nameFilter, const std::vector<std::string> &tags,
                           uint64_t maxResults, std::vector<Hit> &hits, std::mutex &hitsMu,
                           uint64_t &totalOut, bool &truncatedOut) {
    if (!g_driver || !g_driver->ready() || text.empty()) return false;

    std::vector<uint8_t> pattern;
    if (encoding == "utf16" || encoding == "utf16le") {
        for (char c : text) {
            pattern.push_back((uint8_t)c);
            pattern.push_back(0);
        }
    } else {
        pattern.assign(text.begin(), text.end());
    }
    if (pattern.empty()) return false;

    std::vector<Region> regions;
    if (!listRegions(pid, true, regions)) return false;

    std::vector<Region> chosen;
    for (auto &rg : regions) {
        if (!tags.empty()) {
            bool tagMatch = false;
            for (const auto &t : tags) {
                if (strcasecmp(t.c_str(), rg.tag.c_str()) == 0) { tagMatch = true; break; }
            }
            if (!tagMatch) continue;
        }
        if (!nameFilter.empty()) {
            std::string lp = rg.path, lf = nameFilter;
            std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
            std::transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
            if (lp.find(lf) == std::string::npos) continue;
        }
        chosen.push_back(rg);
    }
    if (chosen.empty()) return false;

    size_t patLen = pattern.size();
    size_t kChunk = 1024 * 1024;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk + patLen + 64]);
    if (!buf) return false;

    uint64_t foundTotal = 0;
    bool truncated = false;
    uint64_t bufStart = chosen[0].start;
    size_t bufLen = 0;
    uint64_t scannedThrough = bufStart;

    auto scanBuffer = [&](size_t limit) {
        if (limit < patLen) return;
        size_t maxOff = limit - patLen + 1;
        for (size_t off = 0; off < maxOff; ++off) {
            uint64_t addr = bufStart + off;
            if (addr + patLen <= scannedThrough) continue;
            if (memcmp(buf.get() + off, pattern.data(), patLen) == 0) {
                std::lock_guard<std::mutex> lk(hitsMu);
                hits.push_back(Hit{addr, VT::I8, pattern[0]});
                ++foundTotal;
                if (foundTotal >= maxResults) { truncated = true; return; }
            }
        }
    };

    auto dropTail = [&]() {
        size_t tail = patLen > 1 ? patLen - 1 : 0;
        if (bufLen > tail) {
            memmove(buf.get(), buf.get() + bufLen - tail, tail);
            bufStart += bufLen - tail;
            bufLen = tail;
        }
    };

    for (size_t ri = 0; ri < chosen.size() && !truncated; ++ri) {
        auto &rg = chosen[ri];
        if (ri > 0) { bufStart = rg.start; bufLen = 0; scannedThrough = bufStart; }

        while (!truncated) {
            if (bufStart + bufLen >= rg.end) {
                if (bufLen >= patLen) scanBuffer(bufLen);
                break;
            }
            uint64_t regionLeft = rg.end - (bufStart + bufLen);
            size_t want = (size_t)std::min<uint64_t>(kChunk - bufLen, regionLeft);
            if (want == 0) {
                scanBuffer(bufLen);
                scannedThrough = std::max<uint64_t>(scannedThrough, bufStart + bufLen);
                dropTail();
                if (bufStart + bufLen >= rg.end) break;
                continue;
            }
            if (want < patLen && bufLen < patLen) break;

            size_t got = readContiguous(pid, bufStart + bufLen, buf.get() + bufLen, want);
            if (got == 0) {
                size_t skip = 4096;
                while (skip * 2 <= regionLeft && skip < 16 * 1024 * 1024) {
                    uint8_t p = 0;
                    if (g_driver->readMemCompat(pid, bufStart + bufLen + skip, &p, 1)) break;
                    skip *= 2;
                }
                skip = std::min<size_t>(skip, (size_t)regionLeft);
                bufStart += bufLen + skip;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            bufLen += got;
            scanBuffer(bufLen);
            scannedThrough = bufStart + bufLen;
            if (got < want) {
                bufStart += bufLen;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            dropTail();
        }
    }

    totalOut = foundTotal;
    truncatedOut = truncated;
    return true;
}

// ---------- 通配符特征码扫描 (AOB Scan) ----------

static bool parsePatternString(const std::string &patStr, std::vector<uint8_t> &vals, std::vector<uint8_t> &masks) {
    vals.clear();
    masks.clear();

    std::vector<std::string> tokens;
    // 先检查是否包含空格
    if (patStr.find(' ') != std::string::npos || patStr.find('\t') != std::string::npos) {
        std::string cur;
        for (char c : patStr) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) tokens.push_back(cur);
    } else {
        // 紧凑连续 hex，按每 2 字符切分
        for (size_t i = 0; i < patStr.size(); ) {
            if (patStr[i] == '?') {
                if (i + 1 < patStr.size() && patStr[i + 1] == '?') {
                    tokens.push_back("??");
                    i += 2;
                } else {
                    tokens.push_back("?");
                    i += 1;
                }
            } else if (i + 1 < patStr.size()) {
                tokens.push_back(patStr.substr(i, 2));
                i += 2;
            } else {
                tokens.push_back(patStr.substr(i, 1));
                i += 1;
            }
        }
    }

    if (tokens.empty()) return false;

    for (const auto &tok : tokens) {
        if (tok == "?" || tok == "??" || tok == "*" || tok == "**") {
            vals.push_back(0);
            masks.push_back(0x00);
        } else if (tok.size() == 2 && tok[0] == '?') {
            char b[2] = { tok[1], '\0' };
            uint8_t v = (uint8_t)strtoul(b, nullptr, 16);
            vals.push_back(v);
            masks.push_back(0x0F);
        } else if (tok.size() == 2 && tok[1] == '?') {
            char b[2] = { tok[0], '\0' };
            uint8_t v = (uint8_t)(strtoul(b, nullptr, 16) << 4);
            vals.push_back(v);
            masks.push_back(0xF0);
        } else {
            uint8_t v = (uint8_t)strtoul(tok.c_str(), nullptr, 16);
            vals.push_back(v);
            masks.push_back(0xFF);
        }
    }

    return !vals.empty();
}

bool Scanner::searchPattern(pid_t pid, const std::string &patternStr, size_t align,
                           const std::string &nameFilter, const std::vector<std::string> &tags,
                           uint64_t maxResults, std::vector<Hit> &hits, std::mutex &hitsMu,
                           uint64_t &totalOut, bool &truncatedOut) {
    if (!g_driver || !g_driver->ready() || patternStr.empty()) return false;

    std::vector<uint8_t> pVals, pMasks;
    if (!parsePatternString(patternStr, pVals, pMasks)) return false;
    size_t patLen = pVals.size();

    std::vector<Region> regions;
    if (!listRegions(pid, false, regions)) return false;

    std::vector<Region> chosen;
    for (auto &rg : regions) {
        if (!tags.empty()) {
            bool tagMatch = false;
            for (const auto &t : tags) {
                if (strcasecmp(t.c_str(), rg.tag.c_str()) == 0) { tagMatch = true; break; }
            }
            if (!tagMatch) continue;
        }
        if (!nameFilter.empty()) {
            std::string lp = rg.path, lf = nameFilter;
            std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
            std::transform(lf.begin(), lf.end(), lf.begin(), ::tolower);
            if (lp.find(lf) == std::string::npos) continue;
        }
        chosen.push_back(rg);
    }
    if (chosen.empty()) return false;

    size_t stride = (align == 2 || align == 4 || align == 8) ? align : 1;
    size_t kChunk = 1024 * 1024;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk + patLen + 64]);
    if (!buf) return false;

    uint64_t foundTotal = 0;
    bool truncated = false;
    uint64_t bufStart = chosen[0].start;
    size_t bufLen = 0;
    uint64_t scannedThrough = bufStart;

    auto scanBuffer = [&](size_t limit) {
        if (limit < patLen) return;
        size_t off0 = 0;
        if (stride > 1) off0 = (size_t)((stride - (bufStart % stride)) % stride);
        size_t limitAdj = limit >= patLen ? limit - patLen + 1 : 0;
        for (size_t off = off0; off < limitAdj; off += stride) {
            uint64_t addr = bufStart + off;
            if (addr + patLen <= scannedThrough) continue;
            bool matched = true;
            for (size_t k = 0; k < patLen; ++k) {
                if ((buf[off + k] & pMasks[k]) != pVals[k]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                std::lock_guard<std::mutex> lk(hitsMu);
                hits.push_back(Hit{addr, VT::U32, *(uint32_t *)(buf.get() + off)});
                ++foundTotal;
                if (foundTotal >= maxResults) { truncated = true; return; }
            }
        }
    };

    auto dropTail = [&]() {
        size_t tail = patLen > 1 ? patLen - 1 : 0;
        if (bufLen > tail) {
            memmove(buf.get(), buf.get() + bufLen - tail, tail);
            bufStart += bufLen - tail;
            bufLen = tail;
        }
    };

    for (size_t ri = 0; ri < chosen.size() && !truncated; ++ri) {
        auto &rg = chosen[ri];
        if (ri > 0) { bufStart = rg.start; bufLen = 0; scannedThrough = bufStart; }

        while (!truncated) {
            if (bufStart + bufLen >= rg.end) {
                if (bufLen >= patLen) scanBuffer(bufLen);
                break;
            }
            uint64_t regionLeft = rg.end - (bufStart + bufLen);
            size_t want = (size_t)std::min<uint64_t>(kChunk - bufLen, regionLeft);
            if (want == 0) {
                scanBuffer(bufLen);
                scannedThrough = std::max<uint64_t>(scannedThrough, bufStart + bufLen);
                dropTail();
                if (bufStart + bufLen >= rg.end) break;
                continue;
            }
            if (want < patLen && bufLen < patLen) break;

            size_t got = readContiguous(pid, bufStart + bufLen, buf.get() + bufLen, want);
            if (got == 0) {
                size_t skip = 4096;
                while (skip * 2 <= regionLeft && skip < 16 * 1024 * 1024) {
                    uint8_t p = 0;
                    if (g_driver->readMemCompat(pid, bufStart + bufLen + skip, &p, 1)) break;
                    skip *= 2;
                }
                skip = std::min<size_t>(skip, (size_t)regionLeft);
                bufStart += bufLen + skip;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            bufLen += got;
            scanBuffer(bufLen);
            scannedThrough = bufStart + bufLen;
            if (got < want) {
                bufStart += bufLen;
                bufLen = 0;
                scannedThrough = bufStart;
                continue;
            }
            dropTail();
        }
    }

    totalOut = foundTotal;
    truncatedOut = truncated;
    return true;
}

// ---------- 多级指针链解析 ----------

bool Scanner::resolvePointerChain(pid_t pid, const std::string &baseSpec,
                                  const std::vector<int64_t> &offsets, size_t ptrSize,
                                  VT finalType, PointerChainResult &out) const {
    if (!g_driver || !g_driver->ready() || baseSpec.empty()) {
        out.err = "driver not ready or empty base";
        return false;
    }
    if (ptrSize != 4 && ptrSize != 8) ptrSize = 8;

    uint64_t baseAddr = 0;
    size_t plusPos = baseSpec.find('+');
    if (plusPos != std::string::npos) {
        std::string modName = baseSpec.substr(0, plusPos);
        std::string offStr = baseSpec.substr(plusPos + 1);
        int64_t modOff = (int64_t)strtoull(offStr.c_str(), nullptr, 0);

        // 查找模块基址
        uintptr_t modBase = g_driver->getModuleBase(pid, modName.c_str());
        if (modBase == 0) {
            std::vector<Module> modules;
            listModules(pid, modules);
            for (const auto &m : modules) {
                if (m.name.find(modName) != std::string::npos) {
                    modBase = (uintptr_t)m.base;
                    break;
                }
            }
        }
        if (modBase == 0) {
            std::vector<Region> regions;
            listRegions(pid, false, regions);
            for (const auto &rg : regions) {
                if (rg.path.find(modName) != std::string::npos) {
                    modBase = (uintptr_t)rg.start;
                    break;
                }
            }
        }
        if (modBase == 0) {
            out.err = "base module not found: " + modName;
            return false;
        }
        baseAddr = (uint64_t)modBase + modOff;
    } else {
        baseAddr = strtoull(baseSpec.c_str(), nullptr, 0);
    }

    if (baseAddr == 0) {
        out.err = "invalid base address";
        return false;
    }

    out.baseAddr = baseAddr;
    out.steps.clear();
    uint64_t curPtr = baseAddr;

    for (size_t i = 0; i < offsets.size(); ++i) {
        int64_t off = offsets[i];
        uint64_t stepAddr = curPtr + off;
        bool isLast = (i + 1 == offsets.size());

        if (isLast) {
            uint64_t val = 0;
            bool ok = singleRead(pid, stepAddr, finalType, val);
            out.steps.push_back(PointerChainStep{off, stepAddr, val, ok});
            out.finalAddr = stepAddr;
            out.finalValue = val;
            out.ok = ok;
            if (!ok) out.err = "read final value failed";
            return ok;
        } else {
            uint64_t deref = 0;
            VT pvt = (ptrSize == 4) ? VT::U32 : VT::U64;
            bool ok = singleRead(pid, stepAddr, pvt, deref);
            deref &= 0xFFFFFFFFFFFFULL;
            out.steps.push_back(PointerChainStep{off, stepAddr, deref, ok});
            if (!ok || deref <= 0x10000) {
                out.err = "broken pointer dereference at step " + std::to_string(i);
                out.ok = false;
                return false;
            }
            curPtr = deref;
        }
    }

    out.ok = true;
    return true;
}

// ---------- AArch64 反汇编 ----------

bool Scanner::disassemble(pid_t pid, uint64_t addr, size_t count, std::vector<DisasmLine> &out) const {
    if (!g_driver || !g_driver->ready() || count == 0) return false;
    if (count > 256) count = 256;

    std::vector<Module> modules;
    listModules(pid, modules);

    std::vector<uint32_t> insns(count, 0);
    size_t rd = readContiguous(pid, addr, (uint8_t *)insns.data(), count * 4);
    size_t insnCount = rd / 4;

    out.clear();
    out.reserve(insnCount);

    for (size_t i = 0; i < insnCount; ++i) {
        uint64_t curAddr = addr + i * 4;
        uint32_t raw = insns[i];

        DisasmLine line;
        line.addr = curAddr;
        line.raw = raw;

        char hbuf[32];
        snprintf(hbuf, sizeof(hbuf), "0x%08x", raw);
        line.hex = hbuf;

        decodeArm64(raw, curAddr, line.mnemonic, line.operands);

        for (const auto &m : modules) {
            if (curAddr >= m.base && curAddr < m.end) {
                char sbuf[64];
                snprintf(sbuf, sizeof(sbuf), "+0x%llx", (unsigned long long)(curAddr - m.base));
                size_t slash = m.name.rfind('/');
                std::string baseName = (slash != std::string::npos) ? m.name.substr(slash + 1) : m.name;
                line.symbol = baseName + sbuf;
                break;
            }
        }
        out.push_back(line);
    }

    return !out.empty();
}

// ---------- 内存与模块转储 ----------

bool Scanner::dumpMemory(pid_t pid, uint64_t addr, size_t size, const std::string &outPath,
                         size_t &writtenOut, std::string &err) const {
    if (!g_driver || !g_driver->ready()) { err = "driver not ready"; return false; }
    if (size == 0) { err = "zero size"; return false; }
    FILE *f = fopen(outPath.c_str(), "wb");
    if (!f) { err = "cannot open output file: " + std::string(strerror(errno)); return false; }

    constexpr size_t kChunk = 1024 * 1024;
    std::unique_ptr<uint8_t[]> buf(new (std::nothrow) uint8_t[kChunk]);
    if (!buf) { fclose(f); err = "out of memory"; return false; }

    size_t done = 0;
    while (done < size) {
        size_t want = std::min<size_t>(kChunk, size - done);
        size_t rd = readContiguous(pid, addr + done, buf.get(), want);
        if (rd < want) {
            memset(buf.get() + rd, 0, want - rd);
        }
        if (fwrite(buf.get(), 1, want, f) != want) {
            fclose(f);
            err = "disk write error: " + std::string(strerror(errno));
            return false;
        }
        done += want;
    }
    fclose(f);
    writtenOut = done;
    return true;
}

bool Scanner::dumpModule(pid_t pid, const std::string &moduleName, const std::string &outPath,
                         size_t &writtenOut, std::string &err) const {
    if (!g_driver || !g_driver->ready()) { err = "driver not ready"; return false; }
    std::vector<Module> modules;
    if (!listModules(pid, modules)) { err = "cannot list modules"; return false; }

    const Module *target = nullptr;
    for (const auto &m : modules) {
        if (m.name == moduleName || m.name.find(moduleName) != std::string::npos) {
            target = &m;
            break;
        }
    }
    if (!target) { err = "module not found: " + moduleName; return false; }

    uint64_t base = target->base;
    uint64_t end = target->end;
    if (end <= base) { err = "invalid module range"; return false; }
    size_t totalSize = (size_t)(end - base);

    if (!dumpMemory(pid, base, totalSize, outPath, writtenOut, err)) {
        return false;
    }

    // ELF Program Headers 内存拉伸对齐修正
    FILE *f = fopen(outPath.c_str(), "r+b");
    if (f) {
        uint8_t ident[16] = {};
        if (fread(ident, 1, 16, f) == 16 && ident[0] == 0x7F && ident[1] == 'E' && ident[2] == 'L' && ident[3] == 'F') {
            bool is64 = (ident[4] == 2);
            if (is64) {
                fseek(f, 0x20, SEEK_SET);
                uint64_t phoff = 0;
                uint16_t phentsize = 0, phnum = 0;
                if (fread(&phoff, 8, 1, f) == 1) {
                    fseek(f, 0x36, SEEK_SET);
                    if (fread(&phentsize, 2, 1, f) == 1 && fread(&phnum, 2, 1, f) == 1) {
                        for (uint16_t i = 0; i < phnum; ++i) {
                            long off = (long)(phoff + i * phentsize);
                            fseek(f, off, SEEK_SET);
                            uint32_t p_type = 0;
                            if (fread(&p_type, 4, 1, f) != 1) break;
                            if (p_type == 1 /* PT_LOAD */) {
                                fseek(f, off + 8, SEEK_SET);
                                uint64_t p_offset = 0, p_vaddr = 0, p_paddr = 0, p_filesz = 0, p_memsz = 0;
                                if (fread(&p_offset, 8, 1, f) == 1 &&
                                    fread(&p_vaddr, 8, 1, f) == 1 &&
                                    fread(&p_paddr, 8, 1, f) == 1 &&
                                    fread(&p_filesz, 8, 1, f) == 1 &&
                                    fread(&p_memsz, 8, 1, f) == 1) {
                                    p_offset = p_vaddr;
                                    p_filesz = p_memsz;
                                    fseek(f, off + 8, SEEK_SET);
                                    fwrite(&p_offset, 8, 1, f);
                                    fseek(f, off + 32, SEEK_SET);
                                    fwrite(&p_filesz, 8, 1, f);
                                }
                            }
                        }
                    }
                }
            }
        }
        fclose(f);
    }

    return true;
}

} // namespace eng
