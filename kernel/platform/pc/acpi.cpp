// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <zircon/compiler.h>
#include <assert.h>
#include <err.h>
#include <trace.h>
#include <lk/init.h>
#include <arch/x86/apic.h>
#include <platform/pc/acpi.h>
#include <zircon/types.h>
#include <lib/acpi_lite.h>

#define LOCAL_TRACE 1

/* @brief Enumerate all functioning CPUs and their APIC IDs
 *
 * If apic_ids is NULL, just returns the number of logical processors
 * via num_cpus.
 *
 * @param apic_ids Array to write found APIC ids into.
 * @param len Length of apic_ids.
 * @param num_cpus Output for the number of logical processors detected.
 *
 * @return ZX_OK on success. Note that if len < *num_cpus, not all
 *         logical apic_ids will be returned.
 */
zx_status_t platform_enumerate_cpus(
    uint32_t* apic_ids,
    uint32_t len,
    uint32_t* num_cpus) {
    if (num_cpus == NULL) {
        return ZX_ERR_INVALID_ARGS;
    }

    // for every local apic entry that is enabled, remember the apic id
    uint32_t count = 0;
    acpi_process_madt_entries_etc(ACPI_MADT_TYPE_LOCAL_APIC,
            [apic_ids, &count, len](const void* _entry, size_t entry_len) {
        auto entry = static_cast<const acpi_madt_local_apic_entry*>(_entry);

        if ((entry->flags & ACPI_MADT_FLAG_ENABLED) == 0) {
            return;
        }

        LTRACEF("MADT entry %p: processor id %d apic id %d flags %#x\n",
                entry, entry->processor_id, entry->apic_id, entry->flags);

        if (apic_ids != NULL && count < len) {
            apic_ids[count] = entry->apic_id;
        }
        count++;
    });
    *num_cpus = count;

    return ZX_OK;
}

/* @brief Enumerate all IO APICs
 *
 * If io_apics is NULL, just returns the number of IO APICs
 * via num_io_apics.
 *
 * @param io_apics Array to populate descriptors into.
 * @param len Length of io_apics.
 * @param num_io_apics Number of IO apics found
 *
 * @return ZX_OK on success. Note that if len < *num_io_apics, not all
 *         IO APICs will be returned.
 */
zx_status_t platform_enumerate_io_apics(
    struct io_apic_descriptor* io_apics,
    uint32_t len,
    uint32_t* num_io_apics) {
    if (num_io_apics == NULL) {
        return ZX_ERR_INVALID_ARGS;
    }

    // for every io apic entry, remember some information
    uint32_t count = 0;
    acpi_process_madt_entries_etc(ACPI_MADT_TYPE_IO_APIC,
            [io_apics, &count, len](const void* _entry, size_t entry_len) {
        auto entry = static_cast<const acpi_madt_io_apic_entry*>(_entry);

        LTRACEF("MADT entry %p: apic id %d address %#x irq base %u\n",
                entry, entry->io_apic_id, entry->io_apic_address, entry->global_system_interrupt_base);

        if (io_apics != NULL && count < len) {
            io_apics[count].apic_id = entry->io_apic_id;
            io_apics[count].paddr = entry->io_apic_address;
            io_apics[count].global_irq_base = entry->global_system_interrupt_base;
        }
        count++;
    });
    *num_io_apics = count;

    return ZX_OK;
}

/* @brief Enumerate all interrupt source overrides
 *
 * If isos is NULL, just returns the number of ISOs via num_isos.
 *
 * @param isos Array to populate overrides into.
 * @param len Length of isos.
 * @param num_isos Number of ISOs found
 *
 * @return ZX_OK on success. Note that if len < *num_isos, not all
 *         ISOs will be returned.
 */
zx_status_t platform_enumerate_interrupt_source_overrides(
    struct io_apic_isa_override* isos,
    uint32_t len,
    uint32_t* num_isos) {
    if (num_isos == NULL) {
        return ZX_ERR_INVALID_ARGS;
    }

    uint32_t count = 0;
    acpi_process_madt_entries_etc(ACPI_MADT_TYPE_INT_SOURCE_OVERRIDE,
            [isos, &count, len](const void *_entry, size_t entry_len) {
        auto entry = static_cast<const acpi_madt_int_source_override_entry*>(_entry);

        LTRACEF("MADT entry %p: bus %d source %d gsi %u flags %#x\n",
                entry, entry->bus, entry->source, entry->global_sys_interrupt, entry->flags);
        if (isos != NULL && count < len) {
            if (entry->bus != 0) {
                return; // it must be set to zero, undefined otherwise
            }
            isos[count].isa_irq = entry->source;
            isos[count].remapped = true;
            isos[count].global_irq = entry->global_sys_interrupt;

            uint32_t flags = entry->flags;
            uint32_t polarity = flags & ACPI_MADT_FLAG_POLARITY_MASK;
            uint32_t trigger = flags & ACPI_MADT_FLAG_TRIGGER_MASK;

            // Conforms below means conforms to the bus spec.  ISA is
            // edge triggered and active high.
            switch (polarity) {
            case ACPI_MADT_FLAG_POLARITY_CONFORMS:
            case ACPI_MADT_FLAG_POLARITY_HIGH:
                isos[count].pol = IRQ_POLARITY_ACTIVE_HIGH;
                break;
            case ACPI_MADT_FLAG_POLARITY_LOW:
                isos[count].pol = IRQ_POLARITY_ACTIVE_LOW;
                break;
            default:
                panic("Unknown IRQ polarity in override: %u\n",
                      polarity);
            }

            switch (trigger) {
            case ACPI_MADT_FLAG_TRIGGER_CONFORMS:
            case ACPI_MADT_FLAG_TRIGGER_EDGE:
                isos[count].tm = IRQ_TRIGGER_MODE_EDGE;
                break;
            case ACPI_MADT_FLAG_TRIGGER_LEVEL:
                isos[count].tm = IRQ_TRIGGER_MODE_LEVEL;
                break;
            default:
                panic("Unknown IRQ trigger in override: %u\n",
                      trigger);
            }
        }
    });
    *num_isos = count;

    return ZX_OK;
}

/* @brief Return information about the High Precision Event Timer, if present.
 *
 * @param hpet Descriptor to populate
 *
 * @return ZX_OK on success.
 */
zx_status_t platform_find_hpet(struct acpi_hpet_descriptor* hpet) {
    const acpi_sdt_header* header = acpi_get_table_by_sig(ACPI_HPET_SIG);
    if (!header) {
        return ZX_ERR_NOT_FOUND;
    }

    if (header->revision != 1) {
        return ZX_ERR_NOT_FOUND;
    }

    if (header->length != sizeof(acpi_hpet_table)) {
        return ZX_ERR_NOT_FOUND;
    }

    const acpi_hpet_table* table = reinterpret_cast<const acpi_hpet_table*>(header);

    hpet->minimum_tick = table->minimum_tick;
    hpet->sequence = table->sequence;
    hpet->address = table->address.address;
    switch (table->address.address_space_id) {
    case ACPI_ADDR_SPACE_IO:
        hpet->port_io = true;
        break;
    case ACPI_ADDR_SPACE_MEMORY:
        hpet->port_io = false;
        break;
    default:
        return ZX_ERR_NOT_SUPPORTED;
    }
    TRACE;
    return ZX_OK;
}
