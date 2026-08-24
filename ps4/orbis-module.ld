/* Linker script for a PlayStation 4 libretro module.
 *
 * OpenOrbis's own link.x with ONE correction, and the correction is load-bearing:
 *
 *   .init_array : { *(.init_array); }
 *
 * collects the constructors and defines nothing around them. crtlib.o's module_start walks
 * __init_array_start..__init_array_end - but those two are BSS VARIABLES IN crtlib.o, eight bytes
 * apart and never filled, not linker-provided bounds. Every module built with the stock script
 * reports an empty constructor list.
 *
 * That alone is not what broke C++ cores - module_start is GLOBAL HIDDEN, so it ends up LOCAL and
 * create-fself cannot publish it, and the loader never calls it either way. dylib.c walks the
 * array from the frontend instead. This script is what makes the bounds it reads correct.
 *
 * See libretro-common/dynamic/dylib.c for the whole measurement.
 */
SECTIONS
{
	# Combine text, rodata, and eh_frame stuff
	# In case you'll ever need to get the base address of eboot.bin, you can use the &__text_start symbol.
	# No need to go through libkernel for that.
	.text : ALIGN(0x4000) {
		__text_start = .;
		# "/libexec/ld-elf.so.1"
		QUAD(0x6365786562696C2F);
		QUAD(0x2E666C652D646C2F);
		QUAD(0x00000000312E6F73);
		QUAD(0x0000000000000000);

		# original .text
		*(.text .text.*)
	}

	.rodata : ALIGN(0x10) {
		*(.rodata .rodata.*)
	}

    # Since we lack a proper POSIX dladdr() on a PS4, we define these global symbols
    # For libunwind's bare metal compilation mode.
    # That's way better than going through libkernel... ;-;
	.eh_frame : {
	   __eh_frame_start = .;
	   KEEP(*(.eh_frame))
	   __eh_frame_end = .;
	}

	.eh_frame_hdr : {
	   __eh_frame_hdr_start = .;
	   KEEP(*(.eh_frame_hdr))
	   __eh_frame_hdr_end = .;
	}

	.data.rel.ro : ALIGN(0x4000) {
	   __data_relro_start = .;	
	   KEEP(*(.data.rel.ro .data.rel.ro.*))
	   __data_relro_start = .;
	}
	
	.init_array : {
		__init_array_start = .;
		/* ⚠ .init_array.* AS WELL, AND SORTED. A constructor given a priority lands in
		   .init_array.<NNNNN>, which `*(.init_array)` does not match - so a script that
		   collects only the bare name silently DROPS every prioritised constructor and
		   the module runs with a partial init list. */
		KEEP(*(SORT_BY_INIT_PRIORITY(.init_array.*)));
		KEEP(*(.init_array));
		__init_array_end = .;
	}
	
	.dynamic : {
		*(.dynamic);
	}

	.tls : {
		*(.tdata);
		*(.tbss);
	}

	# Align .got to 0x4000 if .data.rel.ro doesn't exist
	.got : ALIGN(SIZEOF(.data.rel.ro) > 0 ? 8 : 0x4000) {
		*(.got)
	}

	# Align .got.plt to 0x4000 if .got doesn't exist
	.got.plt : ALIGN((SIZEOF(.got) > 0 || SIZEOF(.data.rel.ro) > 0) ? 8 : 0x4000) {
		*(.got.plt)
	}

	.data.sce_process_param : ALIGN(0x4000) {
		KEEP(*(.data.sce_process_param))
	}

	.data.sce_module_param : ALIGN(0x4000) {
		KEEP(*(.data.sce_module_param))
	}

	.data : {
		*(.data)
	}

	.bss : {
		*(.bss .bss.*)
	}

	# Force .got.plt to appear, because SPRX requires a valid .got.plt.
	/DISCARD/ : {
		QUAD(_GLOBAL_OFFSET_TABLE_)
	}
}
