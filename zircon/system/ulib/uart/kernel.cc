// TODO: for using uart::KernelDriver in kernel proper

// IoProvider to map physical MMIO addresses to kernel virtual addresses.
template <typename Config>
struct UartIo : public uart::BasicIoProvider<Config> {
  using Base = uart::BasicIoProvider<Config>;

  static auto MapMmio(uint64_t phys) {
    return reinterpret_cast<volatile void*>(periph_paddr_to_vaddr(phys));
  }

  UartIo(const Config& cfg, uint16_t pio_size) : Base(cfg, pio_size, MapMmio) {}
};

// PIO version doesn't need to map, but it needs to mark the reserved range.
#if defined(__x86_64__) || defined(__i386__)
template <>
struct UartIo<dcfg_simple_pio_t> : public uart::BasicIoProvider<dcfg_simple_pio_t> {
  using Base = uart::BasicIoProvider<dcfg_simple_pio_t>;
  void Init(const Config& cfg, uint16_t pio_size) : Base(cfg, pio_size) {
    mark_pio_region_to_reserve(cfg.base, pio_size);
  }
};
#endif

// Sync for uart::KernelDriver.  Nonblocking version, used early (interrupts
// not set up) and late (panic).  This guards against other CPUs and against
// interrupts, but it does not rely on interrupts.
class TA_CAP("uart") Polling {
 public:
  using InterruptState = spin_lock_saved_state_t;

  static constexpr zx_duration_t kRxInterval = ZX_MSEC(10);

  Polling() = delete;
  template <typename T>
  explicit Polling(const T&) {}

  InterruptState lock() TA_ACQ() {
    InterruptState save;
    spin_lock_irqsave(&lock_, save);
    return save;
  }

  void unlock(InterruptState save) TA_REL() { spin_unlock_irqsave(&lock_, save); }

  template <typename T>
  InterruptState Wait(InterruptState state, T&& enable_tx_interrupt) TA_REQ() {
    unlock(state);
    arch::Yield();
    return lock();
  }

  void Wake() TA_REQ() { ZX_PANIC("should not be used"); }

 protected:
  spin_lock_t lock_ = SPIN_LOCK_INITIAL_VALUE;
};

// Sync for uart::KernelDriver.  Blocking version used in normal kernel
// operation.  This does the same locking, but its Wait actually waits.
class Blocking : public Polling {
 public:
  Blocking() = delete;
  template <typename T>
  explicit Blocking(const T&) {}

  template <typename T>
  InterruptState Wait(InterruptState state, T&& enable_tx_interrupt) TA_REQ() {
    enable_tx_interrupt();
    unlock(state);
    event_.Wait();
    return lock();
  }

  // Called by interrupt handlers.
  void Wake() TA_REQ() { event_.Signal(); }

  template <typename UartDriver>
  static interrupt_eoi IrqHandler(void* arg) {
    auto driver = static_cast<KernelDriver<UartDriver, Blocking, UartIo>*>(arg);
    InterruptGuard lock(driver->sync_);
    driver->uart_.Interrupt(
        driver->uart_.io(), [driver]() { driver->sync_.Wake(); },
        [](uint8_t c) { gConsoleInputBuf.WriteChar(c); });
    return IRQ_EOI_DEACTIVATE;
  }

 private:
  AutounsignalEvent event_{true};

  class TA_SCOPED_CAP InterruptGuard {
   public:
    InterruptGuard() = delete;
    [[nodiscard]] explicit InterruptGuard(Blocking& sync) TA_ACQ() : guard_(&sync.lock_) {}
    ~InterruptGuard() TA_REL() {}

   private:
    AutoSpinLockNoIrqSave guard_;
  };
};
