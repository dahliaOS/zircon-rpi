// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <bits.h>
#include <inttypes.h>
#include <lib/console.h>
#include <platform.h>
#include <trace.h>

#include <arch/arm64/mp.h>
#include <lib/arch/intrin.h>
#include <fbl/alloc_checker.h>
#include <fbl/array.h>
#include <fbl/auto_call.h>
#include <kernel/cpu.h>
#include <lk/init.h>
#include <vm/vm_aspace.h>

#define LOCAL_TRACE 0

namespace {

struct dap_aperture {
  paddr_t base;
  size_t size;
  uint cpu_base;
  void *virt;
};

struct debug_port {
  bool initialized;
  cpu_num_t cpu_num;
  volatile uint32_t *dap;  // pointer to the DAP register window
  volatile uint32_t *cti;  // pointer to the CTI register window
};

fbl::Array<dap_aperture> dap_apertures;
fbl::Array<debug_port> daps;

void arm_dap_init(uint level) {
  LTRACE_ENTRY;

  // TODO: read this from ZBI
  {
    fbl::AllocChecker ac;
    dap_aperture *da = new (&ac) dap_aperture[2]{};
    if (!ac.check()) {
      return;
    }
    dap_apertures = {da, 2};

    // two dap register windows for T931G
    dap_apertures[0].base = 0xf580'0000;  // A53_BASE
    dap_apertures[0].size = 0x80'0000;
    dap_apertures[0].cpu_base = 0;
    dap_apertures[1].base = 0xf500'0000;  // A73_BASE
    dap_apertures[1].size = 0x80'0000;
    dap_apertures[1].cpu_base = 2;
  }

  // allocate a list of parsed dap structures, to be filled in by each cpu as they run their init
  // hook
  {
    fbl::AllocChecker ac;

    debug_port *dp = new (&ac) debug_port[arch_max_num_cpus()]{};
    if (!ac.check()) {
      return;
    }
    daps = {dp, arch_max_num_cpus()};
  }

  // map the dap base into the kernel
  for (auto &da : dap_apertures) {
    LTRACEF("mapping aperture: base %#lx size %#zx cpu base %u\n", da.base, da.size, da.cpu_base);

    zx_status_t err = VmAspace::kernel_aspace()->AllocPhysical(
        "arm dap",
        da.size,          // size
        &da.virt,         // requested virtual vaddress
        PAGE_SIZE_SHIFT,  // alignment log2
        da.base,          // physical vaddress
        0,                // vmm flags
        ARCH_MMU_FLAG_PERM_READ | ARCH_MMU_FLAG_PERM_WRITE |
            ARCH_MMU_FLAG_UNCACHED_DEVICE);  // arch mmu flags
    if (err != ZX_OK) {
      printf("failed to map dap address\n");
      return;
    }

    LTRACEF("dap address %p\n", da.virt);
  }
}

LK_INIT_HOOK(arm_dap, arm_dap_init, LK_INIT_LEVEL_ARCH)

// identify if this is a component according to coresight spec, and return the class id
bool is_component(volatile void *_regs, uint32_t *class_out) {
  volatile uint32_t *regs = static_cast<volatile uint32_t *>(_regs);

  uint32_t cidr[4];
  cidr[0] = regs[0xff0 / 4];
  cidr[1] = regs[0xff4 / 4];
  cidr[2] = regs[0xff8 / 4];
  cidr[3] = regs[0xffc / 4];

  LTRACEF("cidr %#x %#x %#x %#x\n", cidr[0], cidr[1], cidr[2], cidr[3]);

  // does it have a coresight component signature
  if (BITS(cidr[0], 7, 0) != 0xd || BITS(cidr[1], 3, 0) != 0x0 ||  // type 1 rom table
      BITS(cidr[2], 7, 0) != 0x5 || BITS(cidr[3], 7, 0) != 0xb1) {
    return false;
  }

  // read the class nibble
  *class_out = BITS_SHIFT(cidr[1], 7, 4);
  return true;
}

zx_status_t parse_coresight_debug_component(volatile void *_regs) {
  volatile uint32_t *regs = static_cast<volatile uint32_t *>(_regs);

  // are we a coresight component?
  uint32_t class_id;
  if (!is_component(regs, &class_id)) {
    LTRACEF("not a coresight component\n");
    return ZX_ERR_NOT_FOUND;
  }

  if (class_id != 0x9) {
    // not a coresight class
    return ZX_ERR_NOT_FOUND;
  }

  // read common coresight debug regs
  uint32_t devtype = regs[0xfcc / 4];
  uint32_t major = BITS(devtype, 3, 0);
  uint32_t minor = BITS_SHIFT(devtype, 7, 4);
  uint64_t devaff = regs[0xfac / 4];
  devaff = (devaff << 32) | regs[0xfa8 / 4];

  LTRACEF("coresight debug devtype %#x: major %#x minor %#x\n", devtype, major, minor);
  LTRACEF("devaff %#lx, cpu num %u\n", devaff, arm64_mpidr_to_cpu_num(devaff));

  // is this dap for me?
  uint64_t mpidr = __arm_rsr64("mpidr_el1");
  if (mpidr != devaff) {
    return ZX_ERR_NOT_FOUND;
  }
  auto curr_cpu = arch_curr_cpu_num();

#define MAJOR_MINOR(x, y) (((y) << 4 | (x) << 0))
  switch (BITS(devtype, 7, 0)) {
    case MAJOR_MINOR(3, 1):  // trace source, ETM
      // TODO: add CTI
      break;
    case MAJOR_MINOR(4, 1): {  // trigger matrix, CTI
      // TODO: add CTI
      TRACEF("CTI for me: cpu %u base %p\n", curr_cpu, regs);
      debug_port *da = &daps[curr_cpu];
      da->cpu_num = curr_cpu;
      da->cti = regs;
      if (da->cti && da->dap) {
        da->initialized = true;
      }
      break;
    }
    case MAJOR_MINOR(5, 1): {  // DAP per processor core
      // TODO: add dap
      TRACEF("DAP for me: cpu %u base %p\n", curr_cpu, regs);
      debug_port *da = &daps[curr_cpu];
      da->cpu_num = curr_cpu;
      da->dap = regs;
      if (da->cti && da->dap) {
        da->initialized = true;
      }
      break;
    }
    case MAJOR_MINOR(6, 1):  // performance monitor, PMU
      // TODO: add dap
      break;
    default:;
  }
#undef MAJOR_MINOR

  return ZX_OK;
}

zx_status_t parse_rom_table(volatile void *_rom) {
  // start parsing the rom table
  volatile uint32_t *rom = static_cast<volatile uint32_t *>(_rom);

  LTRACEF("parsing rom table at %p\n", rom);

  // is this a rom table?
  uint32_t class_id;
  if (!is_component(rom, &class_id)) {
    LTRACEF("not a coresight component\n");
    return ZX_ERR_NOT_FOUND;
  }

  if (class_id != 1) {
    // not a type 1 rom table
    return ZX_ERR_NOT_FOUND;
  }

  // walk through the rom table until the last possible index or a terminal entry
  for (uint index = 0; index < 0xfd0 / 4; index++) {
    if (rom[index] == 0) {
      // terminal entry
      break;
    }
    LTRACEF("entry %u: %#x\n", index, rom[index]);

    if (BITS(rom[index], 1, 0) != 0x3) {
      // not present or not 32bit
      continue;
    }

    // recurse, seeing if this is a component
    size_t offset = rom[index] & 0xffff'f000;  // mask off bits 11:0
    volatile void *component_virt = (volatile uint8_t *)_rom + offset;
    uint32_t class_id;
    if (!is_component(component_virt, &class_id)) {
      continue;
    }

    LTRACEF("found component at offset %#zx, class %#x\n", offset, class_id);

    // see if we want to recurse
    switch (class_id) {
      case 0x1:  // another type 1 table, recurse
        parse_rom_table(component_virt);
        break;
      case 0x9:  // Coresight Debug component
        parse_coresight_debug_component(component_virt);
        break;
      default:
        LTRACEF("unhandled component class %#x\n", class_id);
    }
  }

  return ZX_OK;
}

// per cpu walk the dap rom tables, looking for debug components associated with this cpu
void arm_dap_init_percpu(uint level) {
  LTRACE_ENTRY;

  LTRACEF("mdrar %#lx\n", __arm_rsr64("mdrar_el1"));
  LTRACEF("dbgauthstatus %#lx\n", __arm_rsr64("dbgauthstatus_el1"));

  for (auto &da : dap_apertures) {
    if (!da.virt) {
      continue;
    }

    // start parsing the rom table
    volatile uint32_t *rom = static_cast<volatile uint32_t *>(da.virt);

    parse_rom_table(rom);
  }
}

LK_INIT_HOOK_FLAGS(arm_dap_percpu, arm_dap_init_percpu, LK_INIT_LEVEL_ARCH + 1,
                   LK_INIT_FLAG_ALL_CPUS)

// helper class to access registers within a memory block
template <typename T>
class RegBlock {
 public:
  RegBlock(volatile uint32_t *regs) : regs_(regs) {}
  ~RegBlock() {}

  void Write(T reg_offset, uint32_t val) {
    regs_[static_cast<size_t>(reg_offset) / 4u] = val;
    arch::DeviceMemoryBarrier();
  }

  uint32_t Read(T reg_offset) {
    uint32_t val = regs_[static_cast<size_t>(reg_offset) / 4u];
    return val;
  }

  zx_status_t WaitFor(T reg_offset, uint32_t mask, uint32_t val, zx_time_t timeout = ZX_SEC(1)) {
    zx_time_t t;
    if (timeout != ZX_TIME_INFINITE) {
      t = current_time();
    }

    uint32_t temp;
    do {
      temp = Read(reg_offset);
      if (timeout != ZX_TIME_INFINITE && (current_time() - t) > timeout) {
        TRACEF("timed out, val %#x\n", temp);
        return ZX_ERR_TIMED_OUT;
      }
    } while ((temp & mask) != val);

    return ZX_OK;
  }

 private:
  volatile uint32_t *regs_;
};

// cti registers (move to top)
enum class cti_regs {
  CTICONTROL = 0x0,
  CTIINTACK = 0x10,
  CTIAPPPULSE = 0x1c,
  CTIOUTEN0 = 0xa0,
  CTIGATE = 0x140,
  CTILAR = 0xfb0,
  CTILSR = 0xfb4,
};

enum class dap_regs {
  DBGDTRRX = 0x80,
  EDITR = 0x84,
  EDSCR = 0x88,
  DBGDTRTX = 0x8c,
  EDRCR = 0x90,
  EDPRSR = 0x314,
  EDLAR = 0xfb0,
  EDLSR = 0xfb4,
  DBGAUTHSTATUS = 0xfb8
};

constexpr uint32_t arm64_nop = 0xd503201f;         // nop
constexpr uint32_t arm64_msr_dbgdtr = 0xd5130400;  // write x0 to dbgdtr
constexpr uint32_t arm64_mrs_dlr = 0xd53b4520;     // write dlr to x0
constexpr uint32_t arm64_mrs_dspsr = 0xd53b4500;   // write dspsr to x0
constexpr uint32_t arm64_mov_sp = 0x910003e0;      // mov sp to x0

zx_status_t run_instruction(RegBlock<dap_regs> &dap, uint32_t instruction, bool trace = false) {
  zx_status_t err;

  if (trace) {
    printf("DAP: running instruction %#x\n", instruction);
  }
  dap.Write(dap_regs::EDRCR, (1 << 3));  // clear the EDSCR.PipeAdv bit

  // wait for EDSCR.PipeAdv == 0 and EDSCR.ITE == 1
  err = dap.WaitFor(dap_regs::EDSCR, (1 << 25) | (1 << 24), (1 << 24));
  if (err != ZX_OK)
    return err;

  // write the instruction
  dap.Write(dap_regs::EDITR, instruction);

  // wait for EDSCR.PipeAdv == 1 and EDSCR.ITE == 1
  // TODO: figure out why pipeadv doesn't always set
  // err = dap.WaitFor(dap_regs::EDSCR, (1<<25) | (1<<24), (1<<25) | (1<<24));
  // if (err != ZX_OK) return err;

  if (trace) {
    printf("DAP: done running instruction %#x\n", instruction);
  }
  return ZX_OK;
}

zx_status_t read_dcc(RegBlock<dap_regs> &dap, uint64_t *val) {
  auto err = dap.WaitFor(dap_regs::EDSCR, (1 << 29), (1 << 29));  // wait for TXFull
  if (err != ZX_OK)
    return err;

  *val = ((uint64_t)dap.Read(dap_regs::DBGDTRRX) << 32) | dap.Read(dap_regs::DBGDTRTX);

  return ZX_OK;
}

struct processor_state {
  uint64_t r[31];
  uint64_t sp;
  uint64_t pc;
  uint64_t cpsr;

  void Dump();
};

void processor_state::Dump() {
  printf("x0  %#18" PRIx64 " x1  %#18" PRIx64 " x2  %#18" PRIx64 " x3  %#18" PRIx64 "\n", r[0],
         r[1], r[2], r[3]);
  printf("x4  %#18" PRIx64 " x5  %#18" PRIx64 " x6  %#18" PRIx64 " x7  %#18" PRIx64 "\n", r[4],
         r[5], r[6], r[7]);
  printf("x8  %#18" PRIx64 " x9  %#18" PRIx64 " x10 %#18" PRIx64 " x11 %#18" PRIx64 "\n", r[8],
         r[9], r[10], r[11]);
  printf("x12 %#18" PRIx64 " x13 %#18" PRIx64 " x14 %#18" PRIx64 " x15 %#18" PRIx64 "\n", r[12],
         r[13], r[14], r[15]);
  printf("x16 %#18" PRIx64 " x17 %#18" PRIx64 " x18 %#18" PRIx64 " x19 %#18" PRIx64 "\n", r[16],
         r[17], r[18], r[19]);
  printf("x20 %#18" PRIx64 " x21 %#18" PRIx64 " x22 %#18" PRIx64 " x23 %#18" PRIx64 "\n", r[20],
         r[21], r[22], r[23]);
  printf("x24 %#18" PRIx64 " x25 %#18" PRIx64 " x26 %#18" PRIx64 " x27 %#18" PRIx64 "\n", r[24],
         r[25], r[26], r[27]);
  printf("x28 %#18" PRIx64 " x29 %#18" PRIx64 " lr  %#18" PRIx64 " sp  %#18" PRIx64 "\n", r[28],
         r[29], r[30], sp);
  printf("pc   %#18" PRIx64 "\n", pc);
  printf("cpsr %#18" PRIx64 "\n", cpsr);
}

zx_status_t read_processor_state(RegBlock<dap_regs> &dap, processor_state *state) {
  zx_status_t err;

  // TODO: save x0 and put it back afterwards

  // read x0 - x30
  for (uint i = 0; i <= 30; i++) {
    err = run_instruction(dap, arm64_msr_dbgdtr | i);
    if (err != ZX_OK)
      return err;

    err = read_dcc(dap, &state->r[i]);
    if (err != ZX_OK)
      return err;
  }

  // read the PC (saved in the DLR_EL0 register)
  err = run_instruction(dap, arm64_mrs_dlr);
  if (err != ZX_OK)
    return err;
  err = run_instruction(dap, arm64_msr_dbgdtr);
  if (err != ZX_OK)
    return err;
  err = read_dcc(dap, &state->pc);
  if (err != ZX_OK)
    return err;

  // save the cpsr of the cpu (saved in DSPSR_EL0)
  err = run_instruction(dap, arm64_mrs_dspsr);
  if (err != ZX_OK)
    return err;
  err = run_instruction(dap, arm64_msr_dbgdtr);
  if (err != ZX_OK)
    return err;
  err = read_dcc(dap, &state->cpsr);
  if (err != ZX_OK)
    return err;

  // save the sp of the cpu
  err = run_instruction(dap, arm64_mov_sp);
  if (err != ZX_OK)
    return err;
  err = run_instruction(dap, arm64_msr_dbgdtr);
  if (err != ZX_OK)
    return err;
  err = read_dcc(dap, &state->sp);
  if (err != ZX_OK)
    return err;

  // TODO: read the EL level and other information from the EDSCR

  return ZX_OK;
}

}  // namespace

// top level entry point, try to drop the victim cpu into debug state and dump the register state
void dap_debug_cpu(cpu_num_t victim) {
  // find the CTI for this cpu
  auto ctir = daps[victim].cti;
  auto dapr = daps[victim].dap;
  if (!ctir || !dapr) {
    return;
  }

  RegBlock<cti_regs> cti(ctir);
  RegBlock<dap_regs> dap(dapr);
  LTRACEF("dbgauthstatus %#x\n", dap.Read(dap_regs::DBGAUTHSTATUS));

  // try to unlock the dap
  LTRACEF("edlsr %#x\n", dap.Read(dap_regs::EDLSR));
  dap.Write(dap_regs::EDLAR, 0xC5ACCE55);
  LTRACEF("edlsr %#x\n", dap.Read(dap_regs::EDLSR));

  // unlock the cti
  LTRACEF("ctilsr %#x\n", cti.Read(cti_regs::CTILSR));
  cti.Write(cti_regs::CTILAR, 0xC5ACCE55);
  LTRACEF("ctilsr %#x\n", cti.Read(cti_regs::CTILSR));

  // enable the cti
  LTRACEF("cticontrol %#x\n", cti.Read(cti_regs::CTICONTROL));
  cti.Write(cti_regs::CTICONTROL, 1);  // make sure CTI is enabled

  // try to put the victim cpu in debug mode
  LTRACEF("ctigate %#x\n", cti.Read(cti_regs::CTIGATE));
  cti.Write(cti_regs::CTIGATE, 0);  // mask off all internal channels
  LTRACEF("ctigate %#x\n", cti.Read(cti_regs::CTIGATE));
  cti.Write(cti_regs::CTIOUTEN0, 1);    // generate input event to channel 0 debug request
  cti.Write(cti_regs::CTIAPPPULSE, 1);  // generate debug event

  // read the status register of the processor
  zx_status_t err = dap.WaitFor(dap_regs::EDPRSR, (1 << 4), (1 << 4));
  if (err != ZX_OK) {
    printf("DAP: failed to drop cpu %u into debug mode\n", victim);
    return;
  }

  // cpu is stopped
  printf("DAP: cpu %u is in debug state\n", victim);

  // ack the CTI
  cti.Write(cti_regs::CTIINTACK, 1);

  // shove a nop down the hole to see if it works
  err = run_instruction(dap, arm64_nop);
  if (err != ZX_OK) {
    printf("DAP: failed to run first instruction on cpu\n");
    return;
  }

  // load the full state of the cpu
  processor_state state = {};
  err = read_processor_state(dap, &state);
  if (err != ZX_OK) {
    printf("DAP: failed to read processor state\n");
    return;
  }

  // dump the state
  state.Dump();
}

namespace {

void test() {
  // pin ourself on one cpu
  Thread::Current::PreemptDisable();

  // reenable preemption on the way out
  auto ac = fbl::MakeAutoCall([]() { Thread::Current::PreemptReenable(); });

  // pick a victim cpu
  cpu_num_t victim = arch_max_num_cpus() - 1;
  if (victim == arch_curr_cpu_num()) {
    victim--;
  }

  printf("curr cpu is %u, victim cpu is %u\n", arch_curr_cpu_num(), victim);

  dap_debug_cpu(victim);
}

void dump() {
  printf("mdrar %#lx\n", __arm_rsr64("mdrar_el1"));
  printf("dbgauthstatus %#lx\n", __arm_rsr64("dbgauthstatus_el1"));

  if (!dap_apertures[0].virt) {
    return;
  }

#if 0
  hexdump(dap_apertures[0].virt, 0x1000);

  hexdump((uint8_t *)dap_apertures[0].virt + 0x1000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x2000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x3000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x4000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x5000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x6000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x400000 + 0xfc0, (0x1000 - 0xfc0));
  hexdump((uint8_t *)dap_apertures[0].virt + 0x400000, 0x1000);
#endif

#if 0
  for (auto &da: dap_apertures) {
    // start parsing the rom table
    volatile uint32_t* rom = static_cast<volatile uint32_t *>(da.virt);

    parse_rom_table(rom);
  }
#endif

  for (auto &da : dap_apertures) {
    printf("DAP aperture at %p, length %#zx\n", da.virt, da.size);
  }

  for (auto &dap : daps) {
    printf("cpu %u DAP %p CTI %p initialized %d\n", dap.cpu_num, dap.dap, dap.cti, dap.initialized);
  }
}

int cmd_dap(int argc, const cmd_args *argv, uint32_t flags) {
  if (argc < 2) {
    // notenoughargs:
    printf("not enough arguments\n");
  usage:
    printf("usage:\n");
    printf("%s dump\n", argv[0].str);
    printf("%s test\n", argv[0].str);
    return ZX_ERR_INTERNAL;
  }

  if (!strcmp(argv[1].str, "dump")) {
    dump();
  } else if (!strcmp(argv[1].str, "test")) {
    test();
  } else {
    printf("unknown command\n");
    goto usage;
  }

  return ZX_OK;
}

#if LK_DEBUGLEVEL > 0
STATIC_COMMAND_START
STATIC_COMMAND("dap", "arm debug port", &cmd_dap)
STATIC_COMMAND_END(dap)
#endif

}  // namespace
