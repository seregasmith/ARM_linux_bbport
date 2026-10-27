#!/usr/bin/env python3
"""Stale buffer reads: region_manager.h, memory_tracker.h and buffer_cache.h (the same edits
already made on the Mac before the connection dropped). Idempotent. Run from port/."""
import sys
from pathlib import Path
done = []
def edit(path, old, new):
    p = Path(path); s = p.read_text()
    if new in s:
        done.append(f'{path}: already there'); return
    n = s.count(old)
    if n != 1:
        sys.exit(f'{path}: expected the original text once, found {n}:\n{old[:200]}')
    p.write_text(s.replace(old, new)); done.append(f'{path}: edited')

RM = 'gpu/shadps4/video_core/buffer_cache/region_manager.h'
edit(RM, """        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
    }""", """        cpu.Fill();
        gpu.Clear();
        writeable.Fill();
        readable.Fill();
        stale_read.Clear();
    }""")
edit(RM, """        if constexpr (type == Type::GPU && enable) {
            NoteGpuBitsSet();
        }
        if constexpr (enable) {""", """        if constexpr (type == Type::GPU && enable) {
            NoteGpuBitsSet();
        }
        if constexpr (type == Type::GPU) {
            // tlg: a new GPU write (or the data back): stale reads end (see AllowStaleRead).
            stale_read.UnsetRange(start_page, end_page);
        }
        if constexpr (enable) {""")
edit(RM, """            } else {
                NoteGpuBitsCleared();
                if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                    UpdateProtection<false, true>();
                }
            }""", """            } else {
                NoteGpuBitsCleared();
                stale_read.UnsetRange(start_page, end_page);
                if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled) {
                    UpdateProtection<false, true>();
                }
            }""")
edit(RM, """    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.""", """    /// tlg: TLG_STALE_BUFFER_READS=1 (BufferCache::TryStaleRead): the CPU may read the GPU
    /// modified pages of the range without waiting for the GPU. They stay GPU modified, so a CPU
    /// write still brings the GPU data back first; a read sees the copy published last (a frame
    /// late). The next GPU write to a page protects it again. True when a page of the range was
    /// protected for reads and no longer is. The caller holds the lock.
    bool AllowStaleRead(VAddr addr, u64 size) {
        const size_t offset = addr - cpu_addr;
        const size_t start_page = SanitizeAddress(offset) / TRACKER_BYTES_PER_PAGE;
        const size_t end_page =
            Common::DivCeil(SanitizeAddress(offset + size), TRACKER_BYTES_PER_PAGE);
        if (start_page >= NUM_PAGES_PER_REGION || end_page <= start_page) {
            return false;
        }
        const RegionBits add(gpu & ~stale_read, start_page, end_page);
        if (add.None()) {
            return false;
        }
        stale_read |= add;
        UpdateProtection<false, true>();
        return true;
    }

    /**
     * Loop over each page in the given range, turn off those bits and notify the tracker if
     * needed. Call the given function on each turned off range.""")
edit(RM, """        RegionBits mask = is_read ? (~gpu ^ readable) : (cpu ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu;
        } else {""", """        // tlg: GPU modified pages are protected for reads unless allowed stale (AllowStaleRead).
        RegionBits mask = is_read ? ((~gpu | stale_read) ^ readable) : (cpu ^ writeable);
        if (mask.None()) {
            return;
        }
        if constexpr (is_read) {
            readable = ~gpu | stale_read;
        } else {""")
edit(RM, """    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
};""", """    RegionBits cpu;
    RegionBits gpu;
    RegionBits writeable;
    RegionBits readable;
    RegionBits stale_read{}; ///< tlg: GPU modified pages the CPU may read stale (AllowStaleRead)
};""")

MT = 'gpu/shadps4/video_core/buffer_cache/memory_tracker.h'
edit(MT, """    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(""", """    /// tlg: see RegionManager::AllowStaleRead (TLG_STALE_BUFFER_READS). True when a page of the
    /// range was protected for reads and no longer is.
    bool AllowStaleRead(VAddr addr, u64 query_size) {
        bool allowed = false;
        IteratePages<false>(addr, query_size, [&allowed](RegionManager* manager, u64 offset, size_t size) {
            std::scoped_lock lk{manager->lock};
            allowed |= manager->AllowStaleRead(manager->GetCpuAddr() + offset, size);
        });
        return allowed;
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(""")
edit(MT, """    /// by someone else and may hold newer CPU writes since: they are not written. Returns pred().
    template <typename Pred, typename Write>
    bool ApplyGpuCopyIf(VAddr cpu_addr, u64 size, Pred&& pred, Write&& write) {""", """    /// by someone else and may hold newer CPU writes since: they are not written. Returns pred().
    /// tlg: unmark = false (stale buffer reads) publishes an outdated copy: the pages are written
    /// the same way but stay GPU modified.
    template <bool unmark = true, typename Pred, typename Write>
    bool ApplyGpuCopyIf(VAddr cpu_addr, u64 size, Pred&& pred, Write&& write) {""")
edit(MT, """                if (in_run) {
                    write(base + run_start, base + offset + seg_size);
                }
                manager->template ChangeRegionState<Type::GPU, false>(base + offset, seg_size);""", """                if (in_run) {
                    write(base + run_start, base + offset + seg_size);
                }
                if constexpr (unmark) {
                    manager->template ChangeRegionState<Type::GPU, false>(base + offset, seg_size);
                }""")

BH = 'gpu/shadps4/video_core/buffer_cache/buffer_cache.h'
edit(BH, """    bool TryEagerRead(VAddr addr, u64 size);
    void NoteReadback(VAddr addr, u64 size);
    void EagerCopyAtFence();
""", """    bool TryEagerRead(VAddr addr, u64 size);
    void NoteReadback(VAddr addr, u64 size);
    void EagerCopyAtFence();

    /// tlg: TLG_STALE_BUFFER_READS=1 (deferred readbacks, precise mode): a CPU read fault on GPU
    /// modified memory does not wait. The pages around it become readable as they are (the copy
    /// published last, about a frame old) while staying GPU modified, and a copy of the current
    /// GPU data is recorded; when the GPU has made it, it is written back (FinishStaleCopy), even
    /// if the GPU wrote the memory again meanwhile (then the pages stay GPU modified). CPU writes
    /// still wait for the GPU data, so no byte the GPU or the CPU wrote is lost; only reads may
    /// see a frame-old version. False: the caller reads back the precise way. Guest threads.
    bool TryStaleRead(VAddr addr, u64 size);
""")
edit(BH, """        std::atomic<bool> stale{};
        std::atomic<bool> applied{};""", """        std::atomic<bool> stale{};
        std::atomic<bool> applied{};
        /// tlg (stale buffer reads): the CPU wrote the memory after the copy was recorded, so
        /// the copy must not be published over it; and a publish is scheduled (keeps the staging).
        std::atomic<bool> cpu_written{};
        std::atomic<bool> publish_pending{};""")
edit(BH, """    void MarkDeferredStale(VAddr addr, u64 size, bool cpu);
""", """    void MarkDeferredStale(VAddr addr, u64 size, bool cpu);
    /// tlg (stale buffer reads): see TryStaleRead.
    static bool StaleReadsEnabled();
    /// Records the copy for a stale read and schedules FinishStaleCopy. GPU-side threads.
    void StartStaleCopy(VAddr addr, u64 size);
    /// Applies the copy (the GPU has made it), or publishes it when the GPU wrote the memory
    /// again since. Any thread.
    void FinishStaleCopy(DeferredReadback& readback);
    /// Writes the bytes of a copy into guest memory for [start, end). Under readback.mutex.
    void WriteDeferredCopy(const DeferredReadback& readback, VAddr start, VAddr end);
    /// The copies in flight overlapping a range the CPU is about to write must not be
    /// published over it. Any thread.
    void MarkDeferredCpuWritten(VAddr addr, u64 size);
""")
print('\n'.join(done))
