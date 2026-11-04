/*
 * Rosalie's Mupen GUI - https://github.com/Rosalie241/RMG
 *  Copyright (C) 2020-2025 Rosalie Wanders <rosalie@mailbox.org>
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 3.
 *  You should have received a copy of the GNU General Public License
 *  along with this program. If not, see <https://www.gnu.org/licenses/>.
 */
#define CORE_INTERNAL
#include "RollbackNetcode.hpp"
#include "Error.hpp"
#include "Library.hpp"
#include "FbneoRuntime.hpp"
#include "LibretroRuntime.hpp"

namespace
{
// FBNeo is the default so N64/arcade sessions keep the exact behaviour they had
// before the libretro runtime existed.
CoreRollbackCore g_core = CoreRollbackCore::Fbneo;

bool runtime_initialized()
{
    switch (g_core)
    {
        case CoreRollbackCore::Libretro:
            return libretro_rt::IsInitialized();
        case CoreRollbackCore::Fbneo:
        default:
            return fbneo_rt::IsInitialized();
    }
}

bool runtime_save_state(unsigned char* buffer, int capacity, int& len, int& checksum, int frame)
{
    switch (g_core)
    {
        case CoreRollbackCore::Libretro:
            return libretro_rt::SaveStateInto(buffer, capacity, len, checksum, frame);
        case CoreRollbackCore::Fbneo:
        default:
            return fbneo_rt::SaveStateInto(buffer, capacity, len, checksum, frame);
    }
}

bool runtime_save_state_alloc(unsigned char*& buffer, int& len, int& checksum, int frame)
{
    switch (g_core)
    {
        case CoreRollbackCore::Libretro:
            return libretro_rt::SaveStateAlloc(buffer, len, checksum, frame);
        case CoreRollbackCore::Fbneo:
        default:
            return fbneo_rt::SaveStateAlloc(buffer, len, checksum, frame);
    }
}

bool runtime_load_state(const unsigned char* buffer, int len)
{
    switch (g_core)
    {
        case CoreRollbackCore::Libretro:
            return libretro_rt::LoadState(buffer, len);
        case CoreRollbackCore::Fbneo:
        default:
            return fbneo_rt::LoadState(buffer, len);
    }
}

bool runtime_run_frame(int flags)
{
    switch (g_core)
    {
        case CoreRollbackCore::Libretro:
            return libretro_rt::RunFrame(flags);
        case CoreRollbackCore::Fbneo:
        default:
            return fbneo_rt::RunFrame(flags);
    }
}
} // namespace

void CoreRollbackSetCore(CoreRollbackCore core)
{
    g_core = core;
}

CoreRollbackCore CoreRollbackGetCore(void)
{
    return g_core;
}

bool CoreRollbackSaveGameState(CoreRollbackState& state, int frame)
{
    return CoreRollbackSaveGameStateInto(state, nullptr, 0, frame);
}

bool CoreRollbackSaveGameStateInto(CoreRollbackState& state, unsigned char* buffer, int capacity, int frame)
{
    std::string error;

    if (!runtime_initialized())
    {
        return false;
    }

    if (buffer != nullptr)
    {
        int len = 0;
        int checksum = 0;
        if (!runtime_save_state(buffer, capacity, len, checksum, frame))
        {
            error = "CoreRollbackSaveGameStateInto runtime SaveStateInto Failed";
            CoreSetError(error);
            return false;
        }
        state.buffer = buffer;
        state.len = len;
        state.checksum = checksum;
        state.frame = frame;
        return true;
    }
    else
    {
        unsigned char* allocBuf = nullptr;
        int len = 0;
        int checksum = 0;
        if (!runtime_save_state_alloc(allocBuf, len, checksum, frame))
        {
            error = "CoreRollbackSaveGameStateInto runtime SaveStateAlloc Failed";
            CoreSetError(error);
            return false;
        }
        state.buffer = allocBuf;
        state.len = len;
        state.checksum = checksum;
        state.frame = frame;
        return true;
    }
}

bool CoreRollbackSaveFullStateInto(CoreRollbackState& state, unsigned char* buffer, int capacity, int frame)
{
    // Both runtimes expose one complete, self-contained serializer state: the
    // stripped/full distinction only exists for FBNeo's BurnStateCompress.
    std::string error;

    if (!runtime_initialized())
    {
        return false;
    }

    if (buffer != nullptr)
    {
        int len = 0;
        int checksum = 0;
        if (!runtime_save_state(buffer, capacity, len, checksum, frame))
        {
            error = "CoreRollbackSaveFullStateInto runtime SaveStateInto Failed";
            CoreSetError(error);
            return false;
        }
        state.buffer = buffer;
        state.len = len;
        state.checksum = checksum;
        state.frame = frame;
        return true;
    }
    else
    {
        unsigned char* allocBuf = nullptr;
        int len = 0;
        int checksum = 0;
        if (!runtime_save_state_alloc(allocBuf, len, checksum, frame))
        {
            error = "CoreRollbackSaveFullStateInto runtime SaveStateAlloc Failed";
            CoreSetError(error);
            return false;
        }
        state.buffer = allocBuf;
        state.len = len;
        state.checksum = checksum;
        state.frame = frame;
        return true;
    }
}

bool CoreRollbackLoadGameState(const CoreRollbackState& state)
{
    std::string error;

    if (!runtime_initialized())
    {
        return false;
    }

    if (!runtime_load_state(state.buffer, state.len))
    {
        error = "CoreRollbackLoadGameState runtime LoadState Failed";
        CoreSetError(error);
        return false;
    }

    return true;
}

bool CoreRollbackLoadGameStateDeferred(const CoreRollbackState& state)
{
    // The libretro runtime has no deferred load: retro_unserialize is synchronous
    // and frame-aligned, which is exactly what the rollback layer needs.
    std::string error;

    if (!runtime_initialized())
    {
        return false;
    }

    if (!runtime_load_state(state.buffer, state.len))
    {
        error = "CoreRollbackLoadGameStateDeferred runtime LoadState Failed";
        CoreSetError(error);
        return false;
    }

    return true;
}

void CoreRollbackFreeGameState(CoreRollbackState& state)
{
    if (state.buffer != nullptr)
    {
        free(state.buffer);
    }

    state = {};
}

bool CoreRollbackAdvanceFrame(void)
{
    return runtime_run_frame(0);
}

bool CoreRollbackSampleInput(void* values, int size, int players)
{
    std::string error;

    if (!runtime_initialized())
    {
        return false;
    }

    const bool ok = (g_core == CoreRollbackCore::Libretro)
        ? libretro_rt::SampleInput(values, size, players)
        : fbneo_rt::SampleInput(values, size, players);

    if (!ok)
    {
        error = "CoreRollbackSampleInput runtime SampleInput Failed";
        CoreSetError(error);
        return false;
    }

    return true;
}

bool CoreRollbackSetInputCallback(CoreRollbackInputCallback callback)
{
    if (!runtime_initialized())
    {
        return false;
    }

    if (g_core == CoreRollbackCore::Libretro)
    {
        libretro_rt::SetInputCallback(callback);
    }
    else
    {
        fbneo_rt::SetInputCallback(callback);
    }
    return true;
}

bool CoreRollbackSetInputPlayers(int players)
{
    if (!runtime_initialized())
    {
        return false;
    }

    if (g_core == CoreRollbackCore::Libretro)
    {
        libretro_rt::SetInputPlayers(players);
    }
    else
    {
        fbneo_rt::SetInputPlayers(players);
    }
    return true;
}

bool CoreRollbackSetDeterministic(bool enabled)
{
    // Determinism is a property of the runtime itself; this only records the
    // request and is safe to call before initialization. Requiring an
    // initialized core here made every fresh session (local or netplay) fail
    // its startup check.
    if (g_core == CoreRollbackCore::Libretro)
    {
        libretro_rt::SetNetgameMode(enabled);
    }
    else
    {
        fbneo_rt::SetDeterministic(enabled);
    }
    return true;
}

bool CoreRollbackSetVerboseStats(bool enabled)
{
    if (!runtime_initialized())
    {
        return false;
    }

    if (g_core == CoreRollbackCore::Fbneo)
    {
        fbneo_rt::SetVerboseStats(enabled);
    }
    return true;
}

bool CoreRollbackSetTimesyncScale(double scale)
{
    if (!runtime_initialized())
    {
        return false;
    }

    if (g_core == CoreRollbackCore::Fbneo)
    {
        fbneo_rt::SetTimesyncScale(scale);
    }
    return true;
}

bool CoreRollbackExecute(m64p_rollback_execute_callbacks& callbacks)
{
    if (!runtime_initialized())
    {
        return false;
    }

    if (g_core == CoreRollbackCore::Libretro)
    {
        return libretro_rt::Execute(callbacks);
    }
    return fbneo_rt::Execute(callbacks);
}

bool CoreRollbackRunFrame(int flags)
{
    if (!runtime_initialized())
    {
        return false;
    }

    return runtime_run_frame(flags);
}

bool CoreRollbackGetRunFrameStats(CoreRollbackRunFrameStats& stats)
{
    if (!runtime_initialized())
    {
        return false;
    }

    m64p_rollback_run_frame_stats coreStats = {};

    if (g_core == CoreRollbackCore::Libretro)
    {
        // The libretro runtime only reports whole-frame time; the mupen64plus
        // dynarec/interrupt counters have no libretro counterpart.
        libretro_rt::GetRunFrameStats(coreStats);
        stats = {};
        stats.totalUs = coreStats.total_us;
        stats.outputFlags = coreStats.output_flags;
        return true;
    }

    fbneo_rt::GetRunFrameStats(coreStats);

    stats.totalUs = coreStats.total_us;    stats.r4300Us = coreStats.r4300_us;
    stats.viUs = coreStats.vi_us;
    stats.newFrameUs = coreStats.new_frame_us;
    stats.cheatsUs = coreStats.cheats_us;
    stats.pacingUs = coreStats.pacing_us;
    stats.inputUs = coreStats.input_us;
    stats.pauseUs = coreStats.pause_us;
    stats.netplayUs = coreStats.netplay_us;
    stats.dynarecRecompileCount = coreStats.dynarec_recompile_count;
    stats.dynarecRecompileUs = coreStats.dynarec_recompile_us;
    stats.dynarecInvalidateUs = coreStats.dynarec_invalidate_us;
    stats.dynarecFullInvalidateCount = coreStats.dynarec_full_invalidate_count;
    stats.dynarecRangeInvalidateCount = coreStats.dynarec_range_invalidate_count;
    stats.dynarecBlockInvalidateCount = coreStats.dynarec_block_invalidate_count;
    stats.dynarecVerifyDirtyCount = coreStats.dynarec_verify_dirty_count;
    stats.dynarecVerifyDirtyUs = coreStats.dynarec_verify_dirty_us;
    stats.dynarecGetAddrCount = coreStats.dynarec_get_addr_count;
    stats.dynarecGetAddrUs = coreStats.dynarec_get_addr_us;
    stats.dynarecGetAddrHtCount = coreStats.dynarec_get_addr_ht_count;
    stats.dynarecGetAddr32Count = coreStats.dynarec_get_addr_32_count;
    stats.dynarecDynamicLinkerCount = coreStats.dynarec_dynamic_linker_count;
    stats.dynarecDynamicLinkerUs = coreStats.dynarec_dynamic_linker_us;
    stats.dynarecDynamicLinkerDsCount = coreStats.dynarec_dynamic_linker_ds_count;
    stats.dynarecDynamicLinkerDsUs = coreStats.dynarec_dynamic_linker_ds_us;
    stats.cachedCodeFullInvalidateCount = coreStats.cached_code_full_invalidate_count;
    stats.cachedCodeRangeInvalidateCount = coreStats.cached_code_range_invalidate_count;
    stats.interruptCount = coreStats.interrupt_count;
    stats.interruptUs = coreStats.interrupt_us;
    stats.interruptMaxUs = coreStats.interrupt_max_us;
    stats.interruptMaxType = coreStats.interrupt_max_type;
    stats.interruptViCount = coreStats.interrupt_vi_count;
    stats.interruptViUs = coreStats.interrupt_vi_us;
    stats.interruptCompareCount = coreStats.interrupt_compare_count;
    stats.interruptCompareUs = coreStats.interrupt_compare_us;
    stats.interruptCheckCount = coreStats.interrupt_check_count;
    stats.interruptCheckUs = coreStats.interrupt_check_us;
    stats.interruptSiCount = coreStats.interrupt_si_count;
    stats.interruptSiUs = coreStats.interrupt_si_us;
    stats.interruptPiCount = coreStats.interrupt_pi_count;
    stats.interruptPiUs = coreStats.interrupt_pi_us;
    stats.interruptAiCount = coreStats.interrupt_ai_count;
    stats.interruptAiUs = coreStats.interrupt_ai_us;
    stats.interruptSpCount = coreStats.interrupt_sp_count;
    stats.interruptSpUs = coreStats.interrupt_sp_us;
    stats.interruptDpCount = coreStats.interrupt_dp_count;
    stats.interruptDpUs = coreStats.interrupt_dp_us;
    stats.interruptRspDmaCount = coreStats.interrupt_rsp_dma_count;
    stats.interruptRspDmaUs = coreStats.interrupt_rsp_dma_us;
    stats.interruptRspTaskCount = coreStats.interrupt_rsp_task_count;
    stats.interruptRspTaskUs = coreStats.interrupt_rsp_task_us;
    stats.aiSetFrequencyCount = coreStats.ai_set_frequency_count;
    stats.aiSetFrequencyUs = coreStats.ai_set_frequency_us;
    stats.aiPushSamplesCount = coreStats.ai_push_samples_count;
    stats.aiPushSamplesUs = coreStats.ai_push_samples_us;
    stats.aiFifoPopCount = coreStats.ai_fifo_pop_count;
    stats.aiFifoPopUs = coreStats.ai_fifo_pop_us;
    stats.aiRaiseInterruptCount = coreStats.ai_raise_interrupt_count;
    stats.aiRaiseInterruptUs = coreStats.ai_raise_interrupt_us;
    stats.emumode = coreStats.emumode;
    stats.cp0CountBefore = coreStats.cp0_count_before;
    stats.cp0CountAfter = coreStats.cp0_count_after;
    stats.nextInterruptBefore = coreStats.next_interrupt_before;
    stats.nextInterruptAfter = coreStats.next_interrupt_after;
    stats.pcBefore = coreStats.pc_before;
    stats.pcAfter = coreStats.pc_after;
    stats.currentFrameBefore = coreStats.current_frame_before;
    stats.currentFrameAfter = coreStats.current_frame_after;
    stats.dynarecPcaddrBefore = coreStats.dynarec_pcaddr_before;
    stats.dynarecPcaddrAfter = coreStats.dynarec_pcaddr_after;
    stats.cp0LastAddrBefore = coreStats.cp0_last_addr_before;
    stats.cp0LastAddrAfter = coreStats.cp0_last_addr_after;
    stats.loadBeforePc = coreStats.load_before_pc;
    stats.loadBeforeCp0Count = coreStats.load_before_cp0_count;
    stats.loadBeforeNextInterrupt = coreStats.load_before_next_interrupt;
    stats.loadBeforeCurrentFrame = coreStats.load_before_current_frame;
    stats.loadProbePc = coreStats.load_probe_pc;
    stats.loadProbeCp0Count = coreStats.load_probe_cp0_count;
    stats.loadProbeNextInterrupt = coreStats.load_probe_next_interrupt;
    stats.loadProbeCurrentFrame = coreStats.load_probe_current_frame;
    stats.hiddenBeginPc = coreStats.hidden_begin_pc;
    stats.hiddenBeginCp0Count = coreStats.hidden_begin_cp0_count;
    stats.hiddenBeginNextInterrupt = coreStats.hidden_begin_next_interrupt;
    stats.hiddenBeginCurrentFrame = coreStats.hidden_begin_current_frame;
    stats.resumeProbePc = coreStats.resume_probe_pc;
    stats.resumeProbeCp0Count = coreStats.resume_probe_cp0_count;
    stats.resumeProbeNextInterrupt = coreStats.resume_probe_next_interrupt;
    stats.resumeProbeCurrentFrame = coreStats.resume_probe_current_frame;
    stats.dynarecCycleCountBefore = coreStats.dynarec_cycle_count_before;
    stats.dynarecCycleCountAfter = coreStats.dynarec_cycle_count_after;
    stats.dynarecPendingExceptionBefore = coreStats.dynarec_pending_exception_before;
    stats.dynarecPendingExceptionAfter = coreStats.dynarec_pending_exception_after;
    stats.dynarecStopBefore = coreStats.dynarec_stop_before;
    stats.dynarecStopAfter = coreStats.dynarec_stop_after;
    stats.loadBeforeCycleCount = coreStats.load_before_cycle_count;
    stats.loadBeforePendingException = coreStats.load_before_pending_exception;
    stats.loadBeforeStop = coreStats.load_before_stop;
    stats.loadProbeCycleCount = coreStats.load_before_cycle_count;
    stats.loadProbePendingException = coreStats.load_probe_pending_exception;
    stats.loadProbeStop = coreStats.load_probe_stop;
    stats.hiddenBeginCycleCount = coreStats.hidden_begin_cycle_count;
    stats.hiddenBeginPendingException = coreStats.hidden_begin_pending_exception;
    stats.hiddenBeginStop = coreStats.hidden_begin_stop;
    stats.resumeProbeCycleCount = coreStats.resume_probe_cycle_count;
    stats.resumeProbePendingException = coreStats.resume_probe_pending_exception;
    stats.resumeProbeStop = coreStats.resume_probe_stop;
    stats.delaySlotBefore = coreStats.delay_slot_before;
    stats.delaySlotAfter = coreStats.delay_slot_after;
    stats.outputFlags = coreStats.output_flags;
    return true;
}
