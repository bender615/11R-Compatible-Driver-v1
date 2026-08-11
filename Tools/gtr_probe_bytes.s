.section __TEXT,__text,regular,pure_instructions
.p2align 2
.globl _probe_sysex_header
_probe_sysex_header:
.incbin "/tmp/gtr_sysex_header.bin"

.p2align 2
.globl _probe_push_ostype
_probe_push_ostype:
.incbin "/tmp/gtr_push_ostype.bin"

.p2align 2
.globl _probe_get_edit_buffer
_probe_get_edit_buffer:
.incbin "/tmp/gtr_get_edit_buffer.bin"

.p2align 2
.globl _probe_message_ctor
_probe_message_ctor:
.incbin "/tmp/gtr_message_ctor.bin"

.p2align 2
.globl _probe_message_dtor
_probe_message_dtor:
.incbin "/tmp/gtr_message_dtor.bin"

.p2align 2
.globl _probe_push_buffer
_probe_push_buffer:
.incbin "/tmp/gtr_push_buffer.bin"

.p2align 2
.globl _probe_pop_buffer
_probe_pop_buffer:
.incbin "/tmp/gtr_pop_buffer.bin"

.p2align 2
.globl _probe_pop_patch_data
_probe_pop_patch_data:
.incbin "/tmp/gtr_pop_patch_data.bin"
