// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_ACPI_LITE_INCLUDE_LIB_ACPI_LITE_H_
#define ZIRCON_KERNEL_LIB_ACPI_LITE_INCLUDE_LIB_ACPI_LITE_H_

#include <stdint.h>
#include <zircon/types.h>

#include <fbl/function.h>

// ACPI structures
struct acpi_rsdp {
  uint8_t sig[8];
  uint8_t checksum;
  uint8_t oemid[6];
  uint8_t revision;
  uint32_t rsdt_address;

  // rev 2+
  uint32_t length;
  uint64_t xsdt_address;
  uint8_t extended_checksum;
  uint8_t reserved[3];
} __PACKED;

#define ACPI_RSDP_SIG "RSD PTR "

// standard system description table header
struct acpi_sdt_header {
  uint8_t sig[4];
  uint32_t length;
  uint8_t revision;
  uint8_t checksum;
  uint8_t oemid[6];
  uint8_t oem_table_id[8];
  uint32_t oem_revision;
  uint32_t creator_id;
  uint32_t creator_revision;
} __PACKED;
static_assert(sizeof(acpi_sdt_header) == 36, "");

struct acpi_rsdt_xsdt {
  acpi_sdt_header header;

  // array of uint32s or uint64 addresses are placed immediately afterwards
  union {
    uint32_t addr32[0];
    uint64_t addr64[0];
  };
} __PACKED;

struct acpi_generic_address_ {
  uint8_t address_space_id;
  uint8_t register_bit_width;
  uint8_t register_bit_offset;
  uint8_t access_size;
  uint64_t address;
} __PACKED;

#define ACPI_ADDR_SPACE_MEMORY 0
#define ACPI_ADDR_SPACE_IO 1

#define ACPI_RSDT_SIG "RSDT"
#define ACPI_XSDT_SIG "XSDT"

struct acpi_madt_table {
  acpi_sdt_header header;

  uint32_t local_int_controller_address;
  uint32_t flags;
} __PACKED;

#define ACPI_MADT_SIG "APIC"

struct acpi_sub_table_header {
  uint8_t type;
  uint8_t length;
} __PACKED;

#define ACPI_HPET_SIG "HPET"
struct acpi_hpet_table {
  acpi_sdt_header header;
  uint32_t id;
  acpi_generic_address_ address;
  uint8_t sequence;
  uint16_t minimum_tick;
  uint8_t flags;
} __PACKED;

// SRAT table and descriptors
#define ACPI_SRAT_SIG "SRAT"
struct acpi_srat_table {
  acpi_sdt_header header;
  uint32_t __reserved;  // should be 1
  uint32_t __reserved2;
} __PACKED;

// type 0: processor local apic/sapic affinity structure
#define ACPI_SRAT_TYPE_PROCESSOR_AFFINITY 0
struct acpi_srat_processor_affinity_entry {
  acpi_sub_table_header header;
  uint8_t proximity_domain_low;
  uint8_t apic_id;
  uint32_t flags;
  uint8_t sapic_eid;
  uint8_t proximity_domain_high[3];
  uint32_t clock_domain;
} __PACKED;
static_assert(sizeof(acpi_srat_processor_affinity_entry) == 16);

#define ACPI_SRAT_FLAG_ENABLED 1

// type 1: memory affinity structure
#define ACPI_SRAT_TYPE_MEMORY_AFFINITY 1
struct acpi_srat_memory_affinity_entry {
  acpi_sub_table_header header;
  uint16_t proximity_domain;
  uint16_t __reserved;
  uint32_t base_address_low;
  uint32_t base_address_high;
  uint32_t length_low;
  uint32_t length_high;
  uint32_t __reserved2;
  uint32_t flags;
  uint32_t __reserved3;
  uint32_t __reserved4;
};
static_assert(sizeof(acpi_srat_memory_affinity_entry) == 40);

// type 2: processor x2apic affinity structure
#define ACPI_SRAT_TYPE_PROCESSOR_X2APIC_AFFINITY 2
struct acpi_srat_processor_x2apic_affinity_entry {
  acpi_sub_table_header header;
  uint16_t __reserved;
  uint32_t proximity_domain;
  uint32_t x2apic_id;
  uint32_t flags;
  uint32_t clock_domain;
  uint32_t __reserved2;
};
static_assert(sizeof(acpi_srat_processor_x2apic_affinity_entry) == 24);

zx_status_t acpi_lite_init(zx_paddr_t rsdt);
void acpi_lite_dump_tables();

const acpi_sdt_header *acpi_get_table_by_sig(const char *sig);
const acpi_sdt_header *acpi_get_table_at_index(size_t index);

// MADT table describes processors and interrupt controllers

// type 0: local apic
#define ACPI_MADT_TYPE_LOCAL_APIC 0
struct acpi_madt_local_apic_entry {
  acpi_sub_table_header header;
  uint8_t processor_id;
  uint8_t apic_id;
  uint32_t flags;
} __PACKED;

#define ACPI_MADT_FLAG_ENABLED 0x1

// type 1: io apic
#define ACPI_MADT_TYPE_IO_APIC 1
struct acpi_madt_io_apic_entry {
  acpi_sub_table_header header;
  uint8_t io_apic_id;
  uint8_t reserved;
  uint32_t io_apic_address;
  uint32_t global_system_interrupt_base;
} __PACKED;

// type 2: interrupt source override
#define ACPI_MADT_TYPE_INT_SOURCE_OVERRIDE 2
struct acpi_madt_int_source_override_entry {
  acpi_sub_table_header header;
  uint8_t bus;
  uint8_t source;
  uint32_t global_sys_interrupt;
  uint16_t flags;
} __PACKED;

#define ACPI_MADT_FLAG_POLARITY_CONFORMS 0b00
#define ACPI_MADT_FLAG_POLARITY_HIGH 0b01
#define ACPI_MADT_FLAG_POLARITY_LOW 0b11
#define ACPI_MADT_FLAG_POLARITY_MASK 0b11

#define ACPI_MADT_FLAG_TRIGGER_CONFORMS 0b0000
#define ACPI_MADT_FLAG_TRIGGER_EDGE 0b0100
#define ACPI_MADT_FLAG_TRIGGER_LEVEL 0b1100
#define ACPI_MADT_FLAG_TRIGGER_MASK 0b1100

// DBG2 table
#define ACPI_DBG2_SIG "DBG2"
struct acpi_dbg2_table {
  acpi_sdt_header header;
  uint32_t offset;
  uint32_t num_entries;
} __PACKED;
static_assert(sizeof(acpi_dbg2_table) == 44);

struct acpi_dbg2_device {
  uint8_t revision;
  uint16_t length;
  uint8_t register_count;
  uint16_t namepath_length;
  uint16_t namepath_offset;
  uint16_t oem_data_length;
  uint16_t oem_data_offset;
  uint16_t port_type;
  uint16_t port_subtype;
  uint16_t reserved;
  uint16_t base_address_offset;
  uint16_t address_size_offset;
} __PACKED;
static_assert(sizeof(acpi_dbg2_device) == 22);

// debug port types
#define ACPI_DBG2_TYPE_SERIAL_PORT 0x8000
#define ACPI_DBG2_TYPE_1394_PORT 0x8001
#define ACPI_DBG2_TYPE_USB_PORT 0x8002
#define ACPI_DBG2_TYPE_NET_PORT 0x8003

// debug port subtypes
#define ACPI_DBG2_SUBTYPE_16550_COMPATIBLE 0x0000
#define ACPI_DBG2_SUBTYPE_16550_SUBSET 0x0001
#define ACPI_DBG2_SUBTYPE_1394_STANDARD 0x0000
#define ACPI_DBG2_SUBTYPE_USB_XHCI 0x0000
#define ACPI_DBG2_SUBTYPE_USB_EHCI 0x0001

// A routine to iterate over all the MADT entries of a particular type via a callback
using MadtEntryCallback = fbl::Function<void(const void *entry, size_t entry_len)>;
zx_status_t acpi_process_madt_entries_etc(const uint8_t search_type, const MadtEntryCallback &);

#endif  // ZIRCON_KERNEL_LIB_ACPI_LITE_INCLUDE_LIB_ACPI_LITE_H_
