/* A hook file's image (tools/hooks.py): code, read-only data, data, each from a page's start, as a port maps them. It begins
   one page in: the port puts a page of its own before it (the header the console's loader reads). */
OUTPUT_FORMAT("elf64-littleaarch64")
OUTPUT_ARCH(aarch64)
ENTRY(nxh_install)
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); dyn PT_DYNAMIC; }
SECTIONS
{
    . = 0x1000;
    __nxh_text = .;
    .text : { KEEP(*(.text.nxh_install)) *(.text .text.*) } :text
    . = ALIGN(0x1000);
    __nxh_rodata = .;
    .rodata : { *(.rodata .rodata.*) } :rodata
    .hash : { *(.hash) } :rodata
    .gnu.hash : { *(.gnu.hash) } :rodata
    .dynsym : { *(.dynsym) } :rodata
    .dynstr : { *(.dynstr) } :rodata
    .rela.dyn : { *(.rela.*) } :rodata
    .eh_frame : { *(.eh_frame*) } :rodata
    . = ALIGN(0x1000);
    __nxh_data = .;
    .data.rel.ro : { *(.data.rel.ro .data.rel.ro.*) } :data
    .dynamic : { *(.dynamic) } :data :dyn
    .got : { *(.got) *(.got.plt) } :data
    nx2_hooks : { __start_nx2_hooks = .; KEEP(*(nx2_hooks)) __stop_nx2_hooks = .; } :data
    .data : { *(.data .data.*) } :data
    . = ALIGN(0x1000);
    __nxh_bss = .;
    .bss (NOLOAD) : { *(.bss .bss.*) *(COMMON) } :data
    . = ALIGN(0x1000);
    __nxh_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.interp) *(.ARM.*) }
}
