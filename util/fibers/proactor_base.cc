// Copyright 2023, Roman Gershman.  All rights reserved.
// See LICENSE for licensing terms.
//

#include "util/fibers/proactor_base.h"

#include <absl/base/attributes.h>
#include <absl/base/internal/cycleclock.h>
#include <signal.h>

#include "base/cycle_clock.h"

#if __linux__
#include <sys/eventfd.h>
constexpr int kNumSig = _NSIG;

#if defined(__aarch64__)
#include <sys/auxv.h>

#ifndef HWCAP_SB
#define HWCAP_SB (1 << 29)
#endif
#endif  // __aarch64__

#else
constexpr int kNumSig = NSIG;
#endif // __linux__

#include <mutex>  // once_flag

#include "base/logging.h"

using namespace std;

namespace util {
namespace fb2 {
namespace {

struct signal_state {
  struct Item {
    ProactorBase* proactor = nullptr;
    std::function<void(int)> cb;
  };

  Item signal_map[kNumSig];
};

signal_state* get_signal_state() {
  static signal_state state;

  return &state;
}

void SigAction(int signal, siginfo_t*, void*) {
  signal_state* state = get_signal_state();
  DCHECK_LT(signal, kNumSig);

  auto& item = state->signal_map[signal];
  auto cb = [signal, &item] { item.cb(signal); };

  if (item.proactor && item.cb) {
    item.proactor->Dispatch(std::move(cb));
  } else {
    LOG(ERROR) << "Tangling signal handler " << signal;
  }
}

#if defined(__aarch64__)
// ARM architecture pause implementation
static inline void arm_arch_pause(void) {
#if defined(__linux__)
  static int use_spin_delay_sb = -1;

  if (use_spin_delay_sb == 1) {
    asm volatile(".inst 0xd50330ff");  // SB instruction encoding
  }
  else if (use_spin_delay_sb == 0) {
    asm volatile("isb");
  }
  else {
    // Initialize variable and check if SB is supported
    if (getauxval(AT_HWCAP) & HWCAP_SB)
      use_spin_delay_sb = 1;
    else
      use_spin_delay_sb = 0;
  }
#else
  asm volatile("isb");
#endif // __linux__
}
#endif // __aarch64__

unsigned pause_amplifier = 50;
uint64_t cycles_per_10us = 1000000;  // correctly defined inside ModuleInit.
std::once_flag module_init;

}  // namespace

// Apparently __thread is more efficient than thread_local when a variable is referenced
// in cc file that does not define it.
__thread ProactorBase::TLInfo ProactorBase::tl_info_;

ProactorBase::ProactorBase() : task_queue_(kTaskQueueLen) {
  call_once(module_init, &ModuleInit);

#ifdef __linux__
  wake_fd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  CHECK_GE(wake_fd_, 0);
  VLOG(1) << "Created wake_fd is " << wake_fd_;
#endif
}

ProactorBase::~ProactorBase() {
#ifdef __linux__
  close(wake_fd_);
#endif

  signal_state* ss = get_signal_state();
  for (size_t i = 0; i < ABSL_ARRAYSIZE(ss->signal_map); ++i) {
    if (ss->signal_map[i].proactor == this) {
      ss->signal_map[i].proactor = nullptr;
      ss->signal_map[i].cb = nullptr;
    }
  }
}

void ProactorBase::Run() {
  VLOG(1) << "ProactorBase::Run";
  CHECK(tl_info_.owner) << "Init was not called";

  SetCustomDispatcher(new ProactorDispatcher(this));

  is_stopped_ = false;
  detail::FiberActive()->Suspend();
}

void ProactorBase::Stop() {
  DispatchBrief([this] { is_stopped_ = true; });
  VLOG(1) << "Proactor::StopFinish";
}

bool ProactorBase::InMyThread() const {
  // This function should not be inline by design.
  // Together with this barrier, it makes sure that pthread_self() is not cached even when
  // the caller stack migrates between threads.
  // See: https://stackoverflow.com/a/75622732
  asm volatile("");

  return pthread_self() == thread_id_;
}

io::MutableBytes ProactorBase::AllocateBuffer(size_t hint_sz) {
  uint8_t* res = new uint8_t[hint_sz];
  return io::MutableBytes{res, hint_sz};
}

void ProactorBase::DeallocateBuffer(io::MutableBytes buf) {
  operator delete[](buf.data());
}

uint32_t ProactorBase::AddOnIdleTask(OnIdleTask f) {
  DCHECK(InMyThread());

  uint32_t res = on_idle_arr_.size();
  on_idle_arr_.push_back(OnIdleWrapper{.task = std::move(f), .next_ts = 0});

  return res;
}

bool ProactorBase::RunOnIdleTasks() {
  if (on_idle_arr_.empty())
    return false;

  uint64_t start = GetClockNanos();
  uint64_t curr_ts = start;

  bool should_spin = false;

  DCHECK_LT(on_idle_next_, on_idle_arr_.size());

  // Perform round robin with on_idle_next_ saving the position between runs.
  do {
    OnIdleWrapper& on_idle = on_idle_arr_[on_idle_next_];

    if (on_idle.task && on_idle.next_ts <= curr_ts) {
      tl_info_.monotonic_time = curr_ts;

      int32_t level = on_idle.task();  // run the task

      if (level < 0) {
        on_idle.task = {};
        if (on_idle_next_ + 1 == on_idle_arr_.size()) {
          do {
            on_idle_arr_.pop_back();
          } while (!on_idle_arr_.empty() && !on_idle_arr_.back().task);
          on_idle_next_ = 0;
          break;  // do/while loop
        }
      } else {   // level >= 0
        curr_ts = GetClockNanos();

        if (unsigned(level) >= kOnIdleMaxLevel) {
          level = kOnIdleMaxLevel;
          should_spin = true;
        } else if (on_idle_next_ <
                   on_idle_arr_.size()) {  // check if the array has not been shrunk.
          uint64_t delta_ns = uint64_t(kIdleCycleMaxMicros) * 1000 / (1 << level);
          on_idle.next_ts = curr_ts + delta_ns;
        }
      }
    }

    ++on_idle_next_;
    if (on_idle_next_ >= on_idle_arr_.size()) {
      on_idle_next_ = 0;
      break;
    }
  } while (curr_ts < start + 10000);  // 10usec for the run.

  return should_spin;
}

bool ProactorBase::RemoveOnIdleTask(uint32_t id) {
  if (id >= on_idle_arr_.size() || !on_idle_arr_[id].task)
    return false;

  on_idle_arr_[id].task = OnIdleTask{};
  while (!on_idle_arr_.back().task) {
    on_idle_arr_.pop_back();
    if (on_idle_arr_.empty())
      break;
  }
  return true;
}

uint32_t ProactorBase::AddPeriodic(uint32_t ms, PeriodicTask f) {
  DCHECK(InMyThread());

  auto id = next_task_id_++;

  PeriodicItem* item = new PeriodicItem;
  item->task = std::move(f);
  item->period.tv_sec = ms / 1000;
  item->period.tv_nsec = (ms % 1000) * 1000000;

  auto [it, inserted] = periodic_map_.emplace(id, item);
  CHECK(inserted);

  SchedulePeriodic(id, item);

  return id;
}

void ProactorBase::CancelPeriodic(uint32_t id) {
  DCHECK(InMyThread());

  auto it = periodic_map_.find(id);
  CHECK(it != periodic_map_.end());
  PeriodicItem* item = it->second;
  --item->ref_cnt;

  // we never deallocate here since there is a callback that holds pointer to the item.
  periodic_map_.erase(it);
  CancelPeriodicInternal(item);
}

void ProactorBase::Migrate(ProactorBase* dest) {
  CHECK(dest != this);
  detail::FiberInterface* fiber = detail::FiberActive();
  fiber->scheduler()->SuspendAndExecuteOnDispatcher([fiber, dest, this] {
    fiber->DetachScheduler();

    auto cb = [fiber] { fiber->AttachScheduler(); };

    // We can not use DispatchBrief because it may block dispatch fiber, which is forbidden.
    // This state is rarely reached in high load situations.
    while (!dest->EmplaceTaskQueue(cb)) {
      tq_full_ev_.fetch_add(1, std::memory_order_relaxed);
      LOG_FIRST_N(WARNING, 10000) << "Retrying task emplace";
      usleep(0);
    };
  });
}

void ProactorBase::RegisterSignal(std::initializer_list<uint16_t> l, ProactorBase* proactor,
                                  std::function<void(int)> cb) {
  CHECK(cb);
  auto* state = get_signal_state();

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = &SigAction;

  for (uint16_t val : l) {
    CHECK(!state->signal_map[val].cb) << "Signal " << val << " was already registered";
    state->signal_map[val].cb = cb;
    state->signal_map[val].proactor = proactor;

    CHECK_EQ(0, sigaction(val, &sa, NULL));
  }
}

void ProactorBase::ClearSignal(std::initializer_list<uint16_t> signals, bool install_ignore) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  sa.sa_handler = install_ignore ? SIG_IGN : SIG_DFL;

  auto* state = get_signal_state();
  for (uint16_t val : signals) {
    // This can happen if say, Proactor is destroyed before the signal is cleared.
    if (!state->signal_map[val].cb) {
      LOG(WARNING) << "Signal " << val << " was already cleared";
      continue;
    }

    state->signal_map[val].cb = nullptr;
    state->signal_map[val].proactor = nullptr;
    CHECK_EQ(0, sigaction(val, &sa, NULL));
  }
}

uint64_t ProactorBase::GetCurrentBusyCycles() const {
  return base::CycleClock::Now() - idle_end_cycle_;
}

// The threshold is set to ~2.5ms.
bool ProactorBase::ShouldPollL2Tasks() const {
  uint64_t now = base::CycleClock::Now();
  return now > last_level2_cycle_ + 256 * cycles_per_10us;
}

bool ProactorBase::RunL2Tasks(detail::Scheduler* scheduler) {
  // avoid calling steady_clock::now() too much.
  // Cycles count can reset, for example when CPU is suspended, therefore we also allow
  // "returning  into past". False positive is possible but it's not a big deal.
  uint64_t now = base::CycleClock::Now();
  if (ABSL_PREDICT_FALSE(now < last_level2_cycle_)) {
    // LOG_FIRST_N - otherwise every adjustment will trigger num-threads messages.
    LOG_FIRST_N(WARNING, 1) << "The cycle clock was adjusted backwards by "
                            << last_level2_cycle_ - now << " cycles";
    now = last_level2_cycle_ + cycles_per_10us;
  }

  bool result = false;
  if (now >= last_level2_cycle_ + cycles_per_10us) {
    last_level2_cycle_ = now;
    scheduler->DestroyTerminated();
    if (scheduler->HasSleepingFibers()) {
      result = scheduler->ProcessSleep() > 0;
    }
  }
  return result;
}

void ProactorBase::IdleEnd(uint64_t start) {
  idle_end_cycle_ = base::CycleClock::Now();

  // Assuming that cpu clock frequency is
  uint64_t kMinCyclePeriod = cycles_per_10us * 500'000ULL;
  cpu_idle_cycles_ += (idle_end_cycle_ - start);

  if (idle_end_cycle_ > cpu_measure_cycle_start_ + kMinCyclePeriod) {
    load_numerator_ = cpu_idle_cycles_;
    load_denominator_ = idle_end_cycle_ - cpu_measure_cycle_start_;
    cpu_idle_cycles_ = 0;
    cpu_measure_cycle_start_ = idle_end_cycle_;
  }
}

void ProactorBase::Pause(unsigned count) {
  auto pc = pause_amplifier;

  for (unsigned i = 0; i < count * pc; ++i) {
#if defined(__i386__) || defined(__amd64__)
    __asm__ __volatile__("pause");
#elif defined(__aarch64__)
    arm_arch_pause();
#endif
  }
}

void ProactorBase::ModuleInit() {
  uint64_t delta;
  cycles_per_10us = base::CycleClock::Frequency() / 100'000;

  while (true) {
    uint64_t now = GetClockNanos();
    for (unsigned i = 0; i < 10; ++i) {
      Pause(kMaxSpinLimit);
    }
    delta = GetClockNanos() - now;
    VLOG(1) << "Running 10 Pause() took " << delta / 1000 << "us";

    if (delta < 2000 || pause_amplifier == 1)  // 2us
      break;
    pause_amplifier -= (pause_amplifier + 7) / 8;
  };
}

void ProactorDispatcher::Run(detail::Scheduler* sched) {
  proactor_->cpu_measure_cycle_start_ = base::CycleClock::Now();
  proactor_->MainLoop(sched);
}

void ProactorDispatcher::Notify() {
  proactor_->WakeupIfNeeded();
}

}  // namespace fb2
}  // namespace util
