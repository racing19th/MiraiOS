BITS 64

DEFAULT REL

global init64:function

extern stackEnd
extern gdtr
extern PML4T
extern PDPT
extern kmain

SECTION .text
init64: ;We are now in 64-bit
	;jump to correct high address
	lea rax, [ABS .cont]
	jmp rax

	.cont:
	;ok now reset stack
	lea rsp, [stackEnd]
	lea rbp, [stackEnd]

	;reload gdtr
	lgdt [gdtr]

	;remove temp PML4T and PDPT entries
	xor rax, rax
	mov [PML4T], rax
	mov [PDPT], rax

	call kmain

	.halt:
		cli
		hlt
		jmp .halt
