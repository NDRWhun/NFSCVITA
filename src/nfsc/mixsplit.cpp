// The audio mixer's SND11 block service (0x89f9fd, a stage-1 job of the System update 0x8993fa) on a worker thread:
// the DAC thread hands each block's service over and renders the voice graph meanwhile, waiting for it where the
// graph copies the service's mix buffer (Snd9Service, 0x8a4d78) and before the next System update. The worker holds
// the System lock around the service as the update does; a service the worker has not started yet runs on the DAC
// thread instead. mixer_split=0 runs the service in place, as does a mixer under the execution lock.

#include "mixsplit.h"
#include "guestcall.h"

#include <lib/memmap.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <native.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sync.h>
#include <recomp.h>
#include <psp2/kernel/threadmgr.h>

#include <atomic>
#include <cstring>

namespace nfsc
{

// Translated bodies of the overridden guest functions; the System lock and unlock.
void sub_8a7de6_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8993fa_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_89f9fd_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8a4d78_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_899352(win32::WinApplication* app, x86::CPU& cpu);
void sub_899369(win32::WinApplication* app, x86::CPU& cpu);

namespace mixsplit
{

namespace
{

constexpr x86::reg32 BlockService = 0x89f9fd;
// The block service's stage job in the Snd9Service instance: link, function, argument, stage (4: on no stage).
constexpr x86::reg32 ServiceJob = 0x24;
constexpr x86::reg32 NoStage = 4;
const x86::reg32 GuestStackSize = 256 * 1024;
const uint32_t HostStackSize = 1024 * 1024;

inline uint32_t readFpscr()
{
    uint32_t value;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(value));
    return value;
}

inline void writeFpscr(uint32_t value)
{
    __asm__ volatile("vmsr fpscr, %0" : : "r"(value) : "memory");
}

// One block's service: the System whose update runs it, the Snd9Service instance, the time argument and the DAC
// thread's state and FPSCR at the call.
struct Job
{
    x86::reg32  system;
    x86::reg32  service;
    x86::reg32  time;
    x86::CPU    cpu;
    uint32_t    fpscr;
};

// The service under the System lock; skipped when its job left the stage list meanwhile.
void runService(win32::WinApplication* app, x86::CPU& cpu, const Job& job)
{
    callGuest(app, cpu, &sub_899352, job.system, {});
    const x86::reg32 node = job.service + ServiceJob;
    if (rc::ld32(node + 4) == BlockService && rc::ld32(node + 8) == job.service && rc::ld32(node + 12) != NoStage)
        callGuest(app, cpu, &sub_89f9fd_guest, cpu.ecx, { job.service, job.time });
    callGuest(app, cpu, &sub_899369, job.system, {});
}

// The worker thread, with its own guest thread block and stack. A job is Posted by the DAC thread, Running once the
// worker or the DAC thread claimed it, Finished when the worker ended it, Idle again once the DAC thread saw that.
class Worker
{
public:
    enum State { Idle, Posted, Running, Finished };

    Worker(win32::WinApplication* app, const x86::CPU& dac, int priority, int cpuMask)
        :   m_app(app)
        ,   m_block(win32::teb::Size)
        ,   m_stack(GuestStackSize)
        ,   m_guestId(rc::ld32(dac.efs + win32::teb::ThreadId))
        ,   m_state(Idle)
        ,   m_waiter(0)
        ,   m_workerUs(0)
        ,   m_stop(false)
        ,   m_go("mixsplit_go", 0)
        ,   m_done("mixsplit_done", 0)
        ,   m_thread("mixsplit", &Worker::entry, this, priority, HostStackSize, cpuMask)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        memset(rc::host(block), 0, teb::Size);
        rc::st32(block + teb::ExceptionList, 0xffffffff);
        rc::st32(block + teb::StackBase, stackTop());
        rc::st32(block + teb::StackLimit, m_stack.getBlockStart());
        rc::st32(block + teb::Self, block);
        rc::st32(block + teb::ProcessId, rc::ld32(dac.efs + teb::ProcessId));
        // The DAC thread's id: the service sees the thread it runs for.
        rc::st32(block + teb::ThreadId, m_guestId);
        app->getMemory<Mutex*>(block + teb::HostLock) = nullptr;
        m_thread.start();
    }

    ~Worker()
    {
        m_stop.store(true, std::memory_order_seq_cst);
        m_go.post();
        m_thread.join();
    }

    // DAC thread: no job open.
    bool idle() const
    {
        return m_state.load(std::memory_order_acquire) == Idle;
    }

    // DAC thread, with no job open: hands the service over.
    void post(const x86::CPU& cpu, x86::reg32 system, x86::reg32 service, x86::reg32 time)
    {
        using namespace win32;
        const x86::reg32 block = m_block.getBlockStart();
        memcpy(rc::host(block + teb::TlsSlots), rc::host(cpu.efs + teb::TlsSlots), 4 * teb::TlsSlotCount);
        rc::st32(block + teb::LastError, rc::ld32(cpu.efs + teb::LastError));
        m_job.system = system;
        m_job.service = service;
        m_job.time = time;
        m_job.cpu = cpu;
        m_job.fpscr = readFpscr();
        m_state.store(Posted, std::memory_order_seq_cst);
        m_go.post();
    }

    // DAC thread: returns once the open job, if any, has run; one the worker has not started runs here.
    void join(win32::WinApplication* app, x86::CPU& cpu)
    {
        int state = m_state.load(std::memory_order_acquire);
        if (state == Idle)
            return;
        const bool timed = pf::perf::enabled();
        if (state == Posted && m_state.compare_exchange_strong(state, Running, std::memory_order_acq_rel))
        {
            const uint32_t start = timed ? pf::perf::now() : 0;
            x86::CPU guest = m_job.cpu;
            guest.efs = cpu.efs;
            guest.esp = cpu.esp;
            runService(app, guest, m_job);
            if (guest.terminate)
                cpu.terminate = true;
            m_state.store(Idle, std::memory_order_release);
            if (timed)
                pf::perf::mixerSplit(0, 0, pf::perf::now() - start);
            return;
        }
        const uint32_t start = timed ? pf::perf::now() : 0;
        if (m_state.load(std::memory_order_seq_cst) != Finished)
        {
            m_waiter.store(1, std::memory_order_seq_cst);
            // Either the worker sees the flag and posts, or it has finished and the flag is taken back here.
            if (m_state.load(std::memory_order_seq_cst) != Finished || m_waiter.exchange(0) == 0)
                m_done.wait();
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (m_terminate)
            cpu.terminate = true;
        m_state.store(Idle, std::memory_order_release);
        if (timed)
            pf::perf::mixerSplit(m_workerUs, pf::perf::now() - start, 0);
    }

private:
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    x86::reg32 stackTop() const
    {
        return m_stack.getBlockStart() + m_stack.getBlockSize();
    }

    static int entry(void* self)
    {
        static_cast<Worker*>(self)->loop();
        return 0;
    }

    void loop()
    {
        using namespace win32;
        ThreadControl* const control = scheduler::setCurrentWorkerThread(m_guestId, "mixwork");
        m_app->getMemory<ThreadControl*>(m_block.getBlockStart() + teb::HostControl) = control;
        for (;;)
        {
            m_go.wait();
            if (m_stop.load(std::memory_order_seq_cst))
                break;
            int expected = Posted;
            if (!m_state.compare_exchange_strong(expected, Running, std::memory_order_acq_rel))
                continue;
            const uint32_t start = pf::perf::enabled() ? pf::perf::now() : 0;
            writeFpscr(m_job.fpscr);
            m_cpu = m_job.cpu;
            m_cpu.efs = m_block.getBlockStart();
            // An empty return address slot at the top of the worker's stack.
            m_cpu.esp = stackTop() - 16;
            rc::st32(m_cpu.esp, 0);
            m_cpu.terminate = false;
            scheduler::watchFree(control, m_cpu);
            runService(m_app, m_cpu, m_job);
            scheduler::watchState(control, scheduler::WatchIdle);
            m_terminate = m_cpu.terminate;
            m_workerUs = pf::perf::enabled() ? pf::perf::now() - start : 0;
            m_state.store(Finished, std::memory_order_seq_cst);
            if (m_waiter.exchange(0) == 1)
                m_done.post();
        }
    }

    win32::WinApplication*  m_app;
    win32::MemMap           m_block;
    win32::MemMap           m_stack;
    const x86::reg32        m_guestId;
    Job                     m_job;
    // The worker's guest state; the hang watchdog reads it.
    x86::CPU                m_cpu;
    std::atomic<int>        m_state;
    std::atomic<int>        m_waiter;
    uint32_t                m_workerUs;
    bool                    m_terminate = false;
    std::atomic<bool>       m_stop;
    pf::Semaphore           m_go;
    pf::Semaphore           m_done;
    pf::Thread              m_thread;
};

// DAC thread state, written on that thread only.
bool s_enabled = false;
x86::reg32 s_dacBlock = 0;
// The System whose update runs on the DAC thread now.
x86::reg32 s_system = 0;
Worker* s_worker = nullptr;
// Read by other threads (audio_parallel).
std::atomic<int> s_workerPriority{0};

bool onDac(const x86::CPU& cpu)
{
    return s_enabled && cpu.efs == s_dacBlock;
}

void join(win32::WinApplication* app, x86::CPU& cpu)
{
    if (s_worker)
        s_worker->join(app, cpu);
}

// With core 3 the worker has it, at the DAC thread's priority. Without it the worker may run on cores 0-2 below the
// game and display threads: it takes the idle time of cores 0 and 2, and core 1 while the DAC thread waits for it.
Worker* createWorker(win32::WinApplication* app, const x86::CPU& cpu)
{
    const bool core3 = pf::cores::hasCore3();
    const int mask = core3 ? pf::Thread::Cpu3 : pf::Thread::CpuAll;
    const int priority = core3 ? sceKernelGetThreadCurrentPriority() : win32::scheduler::hostPriorityOf(0) + 1;
    s_workerPriority.store(priority, std::memory_order_relaxed);
    if (pf::perf::enabled())
        pf::logInfo("mixsplit: the SND11 block service runs on a worker thread on core%s, priority %d",
                    core3 ? " 3" : "s 0-2", priority);
    return new Worker(app, cpu, priority, mask);
}

}

int workerPriority()
{
    return s_workerPriority.load(std::memory_order_relaxed);
}

}

namespace native
{

// RWAudioCore Dac thread body: the split starts here and ends with it.
void sub_8a7de6(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace mixsplit;
    // Only beside a mixer that runs without the execution lock (mixer_parallel).
    const win32::Mutex* const executionLock = app->getMemory<win32::Mutex*>(cpu.efs + win32::teb::HostLock);
    s_enabled = pf::Config::getBool("mixer_split", true) && !executionLock;
    s_dacBlock = cpu.efs;
    sub_8a7de6_guest(app, cpu);
    join(app, cpu);
    delete s_worker;
    s_worker = nullptr;
    s_enabled = false;
}

// System update: a block service still open ends before it.
void sub_8993fa(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace mixsplit;
    if (onDac(cpu))
    {
        join(app, cpu);
        s_system = cpu.ecx;
    }
    sub_8993fa_guest(app, cpu);
    if (onDac(cpu))
        s_system = 0;
}

// SND11 block service (stage-1 job, cdecl: Snd9Service instance, time): handed to the worker during the DAC thread's
// System update. A second service in the same update runs in place: the open one waits for the lock held here.
void sub_89f9fd(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace mixsplit;
    if (onDac(cpu) && s_system && (!s_worker || s_worker->idle()))
    {
        if (!s_worker)
            s_worker = createWorker(app, cpu);
        s_worker->post(cpu, s_system, rc::ld32(cpu.esp + 4), rc::ld32(cpu.esp + 8));
        cpu.esp += 4;
        return;
    }
    sub_89f9fd_guest(app, cpu);
}

// Snd9Service graph stage: copies the block service's mix buffer into the graph, so the service must have run.
void sub_8a4d78(win32::WinApplication* app, x86::CPU& cpu)
{
    using namespace mixsplit;
    if (onDac(cpu))
        join(app, cpu);
    sub_8a4d78_guest(app, cpu);
}

}
}
