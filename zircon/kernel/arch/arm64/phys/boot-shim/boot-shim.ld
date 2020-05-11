/* Copyright 2020 The Fuchsia Authors
 *
 * Use of this source code is governed by a MIT-style
 * license that can be found in the LICENSE file or at
 * https://opensource.org/licenses/MIT
 */

/*
 * This is based on kernel/phys/phys.ld and should be updated to match it.
 * Only the `embedded_zbi` bits are added here, and the entry symbol changed.
 *
 * This expects KERNEL_ALIGN to be set via -defsym (see BUILD.gn).
 */

SECTIONS {
  /*
   * A phys executable is purely position-independent so its link address
   * doesn't really matter to it--it only affects the debug file, not the
   * load image.  Using zero simplifies the arithmetic when debugging.
   *
   * It's all a single contiguous segment that is both writable and
   * executable, since no MMU protections are enabled.
   */

  . = 0;
  PROVIDE_HIDDEN(__code_start = .);

  .boot.header : {
    *(.boot.header)
  } :load

  .text : {
    *(.text*)
  }

  .note.gnu.build-id : {
    PROVIDE_HIDDEN(__start_note_gnu_build_id = .);
    *(.note.gnu.build-id)
    PROVIDE_HIDDEN(__stop_note_gnu_build_id = .);
  } :load :note

  .rodata : {
    *(.rodata* .data.rel.ro*)
  } :load

  .preinit_array : {
    KEEP(*(.preinit_array))
  }
  ASSERT(SIZEOF(.preinit_array) == 0, "no static constructors allowed")

  .init_array : {
    KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)) *(SORT_BY_INIT_PRIORITY(.ctors.*)))
    KEEP(*(.init_array))
  }
  ASSERT(SIZEOF(.init_array) == 0, "no static constructors allowed")

  .data : {
    *(.data*)
  }

  .bss : {
    /*
     * Despite the name, _edata really represents the start of bss rather than
     * the end of data.  Putting it here gives it the alignment of the output
     * .bss section so the start.S code can rely on its alignment.
     */
    PROVIDE_HIDDEN(_edata = .);

    *(.bss* .sbss* COMMON)

    /*
     * The start.S code relies on the alignment of the end of .bss as well.
     * This implicitly ensures the alignment of the start of the section
     * is at least this much.
     */
    . = ALIGN(16);
    PROVIDE_HIDDEN(_end = .);
  }

  /*
   * Pad the shim image out to the alignment the kernel requires.
   * A complete arm64 ZBI can be appended directly to the shim.
   */
  PROVIDE_HIDDEN(embedded_zbi = ALIGN(KERNEL_ALIGN));

  /*
   * We need some nonzero contents to prevent the linker from treating
   * this as part of bss, i.e. not actually in the image.  The magic
   * number is 'BOOTSHIM' in little-endian, but nothing should care.
   */
  .pad embedded_zbi - 8 : {
    QUAD(0x4d494853544f4f42);
    ASSERT(ABSOLUTE(.) == ABSOLUTE(embedded_zbi), "shim size check");
  }
}

PHDRS {
  load PT_LOAD FLAGS(7);  /* PF_R|PF_W|PF_X */
  note PT_NOTE FLAGS(4);  /* PF_R */
}

/*
 * This has no real effect since the ELF headers aren't used at runtime.
 * But it makes e_entry in the debug file match the entry point address.
 */
ENTRY(_uefi_start)
