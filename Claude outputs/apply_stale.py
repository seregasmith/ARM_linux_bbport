#!/usr/bin/env python3
"""Stale buffer reads (TLG_STALE_BUFFER_READS): the edits outside region_manager.h and
memory_tracker.h. Idempotent: an edit already in place is skipped. Run from port/."""
import sys
from pathlib import Path

done = []

def edit(path, old, new):
    p = Path(path)
    s = p.read_text()
    if new in s:
        done.append(f'{path}: already there')
        return
    n = s.count(old)
    if n != 1:
        sys.exit(f'{path}: expected the original text once, found {n}:\n{old[:200]}')
    p.write_text(s.replace(old, new))
    done.append(f'{path}: edited')

BC = 'gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp'

edit(BC, """    std::atomic<u64> guest_waits{0}, guest_wait_ns{0};
    std::atomic<s64> window_start{0};
};""", """    std::atomic<u64> guest_waits{0}, guest_wait_ns{0};
    /// tlg (TLG_STALE_BUFFER_READS): read faults served without waiting; their copies applied
    /// (the GPU had not written the memory again), published a frame late (it had), or dropped
    /// (the CPU wrote the memory since).
    std::atomic<u64> stale_served{0}, stale_fresh{0}, stale_late{0}, stale_dropped{0};
    std::atomic<s64> window_start{0};
};""")

edit(BC, """    std::printf("  waits for a readback copy: %llu, %.1f ms\\n",
                static_cast<unsigned long long>(r.guest_waits.exchange(0)),
                r.guest_wait_ns.exchange(0) / 1e6);""", """    std::printf("  waits for a readback copy: %llu, %.1f ms; stale reads: %llu served without waiting, "
                "%llu copies applied, %llu published a frame late, %llu dropped\\n",
                static_cast<unsigned long long>(r.guest_waits.exchange(0)),
                r.guest_wait_ns.exchange(0) / 1e6,
                static_cast<unsigned long long>(r.stale_served.exchange(0)),
                static_cast<unsigned long long>(r.stale_fresh.exchange(0)),
                static_cast<unsigned long long>(r.stale_late.exchange(0)),
                static_cast<unsigned long long>(r.stale_dropped.exchange(0)));""")

edit(BC, """void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    TlgPerf::Span trace_read{""", """void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks) {
    if (is_write && StaleReadsEnabled()) {
        // A stale read's copy recorded before this write must not be published over it.
        MarkDeferredCpuWritten(device_addr, size);
    }
    TlgPerf::Span trace_read{""")

edit(BC, """    readback.staging.Invalidate();
    const u8* const mapped = readback.staging.mapped - readback.staging.offset;
    const bool applied = memory_tracker->ApplyGpuCopyIf(
        readback.start, readback.end - readback.start, [&] { return !readback.stale.load(); },
        [&](VAddr start, VAddr end) {
            for (const auto& copy : readback.copies) {
                const VAddr copy_start = readback.arena_base + copy.srcOffset;
                const VAddr lo = std::max<VAddr>(copy_start, start);
                const VAddr hi = std::min<VAddr>(copy_start + copy.size, end);
                if (lo < hi) {
                    memory->TryWriteBacking(reinterpret_cast<void*>(lo),
                                            mapped + copy.dstOffset + (lo - copy_start), hi - lo);
                }
            }
        });
    if (!applied) {""", """    readback.staging.Invalidate();
    const bool applied = memory_tracker->ApplyGpuCopyIf(
        readback.start, readback.end - readback.start, [&] { return !readback.stale.load(); },
        [&](VAddr start, VAddr end) { WriteDeferredCopy(readback, start, end); });
    if (!applied) {""")

edit(BC, """    std::erase_if(deferred_readbacks, [this](const std::shared_ptr<DeferredReadback>& readback) {
        if (!readback->Done()) {
            return false;
        }""", """    std::erase_if(deferred_readbacks, [this](const std::shared_ptr<DeferredReadback>& readback) {
        // tlg: a stale read's publish still to come reads the staging memory.
        if (!readback->Done() || readback->publish_pending.load(std::memory_order_acquire)) {
            return false;
        }""")

edit(BC, """            if (!readback->Done() && readback->start < addr + size && addr < readback->end) {
                readback->stale.store(true);
                if (cpu) {
                    marked.push_back(readback);
                }
            }""", """            if (!readback->Done() && readback->start < addr + size && addr < readback->end) {
                readback->stale.store(true);
                if (cpu) {
                    readback->cpu_written.store(true);
                    marked.push_back(readback);
                }
            }""")

NEW_FUNCTIONS = r'''
// tlg: TLG_STALE_BUFFER_READS=1, see BufferCache::TryStaleRead (buffer_cache.h).
bool BufferCache::StaleReadsEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_STALE_BUFFER_READS");
        if (!e || *e != '1') {
            return false;
        }
        if (!DeferredReadbackEnabled()) {
            std::printf("GPU: TLG_STALE_BUFFER_READS=1 needs GPU accuracy Precise and deferred "
                        "readbacks: off\n");
            return false;
        }
        std::printf("GPU: stale buffer reads on (TLG_STALE_BUFFER_READS=1): the game reads GPU "
                    "results without waiting, about a frame late\n");
        return true;
    }();
    return enabled;
}

bool BufferCache::TryStaleRead(VAddr addr, u64 size) {
    if (!StaleReadsEnabled()) {
        return false;
    }
    // The faulting page first: when this cache did not protect it (another watcher does, or a
    // GPU write raced the fault) the caller reads it back the precise way, so a fault never
    // comes back here for the same page.
    if (!memory_tracker->AllowStaleRead(addr, size)) {
        return false;
    }
    // The rest of the readback window as well: the copy below covers all of it.
    const auto window = GetReadbackWindow(addr, size);
    const VAddr window_start = std::get<1>(window);
    const VAddr window_end = std::get<2>(window);
    memory_tracker->AllowStaleRead(window_start, window_end - window_start);
    // A copy in flight that covers it brings the data anyway (whoever recorded it applies it).
    if (!PendingDeferredReadback(addr, size)) {
        liverpool->SendCommand<false, true>([this, addr, size] { StartStaleCopy(addr, size); });
    }
    if (ReadbackStatsEnabled()) {
        Readbacks().stale_served.fetch_add(1);
        ReportReadbacks();
    }
    return true;
}

void BufferCache::StartStaleCopy(VAddr addr, u64 size) {
    const auto readback = StartDeferredReadback(addr, size);
    if (!readback || readback->Done() || readback->publish_pending.exchange(true)) {
        return; // nothing GPU modified left to copy, or a publish is already scheduled
    }
    scheduler.DeferPriorityOperation([this, readback] { FinishStaleCopy(*readback); },
                                     readback->tick);
}

void BufferCache::FinishStaleCopy(DeferredReadback& readback) {
    if (FinishDeferredReadback(readback, false)) {
        if (ReadbackStatsEnabled()) {
            Readbacks().stale_fresh.fetch_add(1);
        }
    } else {
        // The GPU wrote the memory again after the copy (stale), or the copy was given up
        // (recovered): its bytes are still newer than the guest's. They are written with the
        // pages left GPU modified (the GPU has newer data), unless the CPU wrote there since.
        bool published = false;
        {
            std::scoped_lock lock{readback.mutex};
            if (!readback.applied.load(std::memory_order_acquire) &&
                !readback.cpu_written.load(std::memory_order_acquire)) {
                readback.staging.Invalidate();
                published = memory_tracker->ApplyGpuCopyIf<false>(
                    readback.start, readback.end - readback.start,
                    [&] { return !readback.cpu_written.load(); },
                    [&](VAddr start, VAddr end) { WriteDeferredCopy(readback, start, end); });
            }
        }
        if (ReadbackStatsEnabled()) {
            (published ? Readbacks().stale_late : Readbacks().stale_dropped).fetch_add(1);
        }
    }
    readback.publish_pending.store(false, std::memory_order_release);
}

void BufferCache::WriteDeferredCopy(const DeferredReadback& readback, VAddr start, VAddr end) {
    const u8* const mapped = readback.staging.mapped - readback.staging.offset;
    for (const auto& copy : readback.copies) {
        const VAddr copy_start = readback.arena_base + copy.srcOffset;
        const VAddr lo = std::max<VAddr>(copy_start, start);
        const VAddr hi = std::min<VAddr>(copy_start + copy.size, end);
        if (lo < hi) {
            memory->TryWriteBacking(reinterpret_cast<void*>(lo),
                                    mapped + copy.dstOffset + (lo - copy_start), hi - lo);
        }
    }
}

void BufferCache::MarkDeferredCpuWritten(VAddr addr, u64 size) {
    if (!deferred_count.load(std::memory_order_acquire)) {
        return;
    }
    boost::container::small_vector<std::shared_ptr<DeferredReadback>, 4> marked;
    {
        std::scoped_lock lock{deferred_mutex};
        for (const auto& readback : deferred_readbacks) {
            if ((!readback->Done() || readback->publish_pending.load(std::memory_order_acquire)) &&
                readback->start < addr + size && addr < readback->end) {
                readback->cpu_written.store(true);
                marked.push_back(readback);
            }
        }
    }
    // A publish already past its check finishes before the guest writes.
    for (const auto& readback : marked) {
        std::scoped_lock lock{readback->mutex};
    }
}
'''

s = Path(BC).read_text()
if 'bool BufferCache::TryStaleRead(' in s:
    done.append(f'{BC}: functions already there')
else:
    anchor = '\nbool BufferCache::CpuWriteMerge('
    if s.count(anchor) != 1:
        sys.exit(f'{BC}: CpuWriteMerge not found once')
    Path(BC).write_text(s.replace(anchor, NEW_FUNCTIONS + anchor))
    done.append(f'{BC}: functions added')

edit('gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h', """        priority_pending_ops_cv.notify_one();
    }
""", """        priority_pending_ops_cv.notify_one();
    }

    /// tlg: as DeferPriorityOperation, once the GPU has reached `tick` (stale buffer reads,
    /// BufferCache::StartStaleCopy).
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func, u64 tick) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), tick);
        }
        priority_pending_ops_cv.notify_one();
    }
""")

edit('gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp', """    } else if (!assume_locks && !image_readback && buffer_cache.TryEagerRead(addr, size)) {
        // tlg: served from the copy made at the last fence (TLG_EAGER_READBACK). Not after an
        // image readback: that resolved newer data the buffer copy would overwrite.
    } else if""", """    } else if (!assume_locks && !image_readback && buffer_cache.TryEagerRead(addr, size)) {
        // tlg: served from the copy made at the last fence (TLG_EAGER_READBACK). Not after an
        // image readback: that resolved newer data the buffer copy would overwrite.
    } else if (!assume_locks && !image_readback && buffer_cache.TryStaleRead(addr, size)) {
        // tlg: TLG_STALE_BUFFER_READS=1: read as it is (about a frame old) without waiting;
        // the current GPU data follows when the GPU has copied it (BufferCache::TryStaleRead).
    } else if""")

edit('launcher/advanced_table.py', """    ('read', 'WRITE_MERGE', 'bool', '1', ('Merge game writes', 'Juntar escritas do jogo'),""", """    ('read', 'STALE_BUFFER_READS', 'bool', '0', ('Read GPU results a frame late', 'Ler resultados da GPU com um quadro de atraso'),
     ('Experimental, needs "Precise" GPU accuracy: when the game reads memory the GPU wrote, it gets the version '
      'from about a frame earlier at once instead of waiting for the GPU. Fewer stalls; some effects may lag a frame.',
      'Experimental, precisa da precisão "Preciso": quando o jogo lê memória que a GPU escreveu, recebe na hora a '
      'versão de cerca de um quadro antes em vez de esperar a GPU. Menos travadas; alguns efeitos podem atrasar um quadro.'),
     {'risky': True}),
    ('read', 'WRITE_MERGE', 'bool', '1', ('Merge game writes', 'Juntar escritas do jogo'),""")

print('\n'.join(done))
