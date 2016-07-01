PIC1		equ 0x20
PIC2		equ 0xA0
PIC1_COMMAND	equ PIC1
PIC1_DATA	equ (PIC1 + 1)
PIC2_COMMAND	equ PIC2
PIC2_DATA	equ (PIC2 + 1)

SECTION .text
global inb:function
inb:		;(short port)
		push ebp
		mov ebp, esp
		xor eax, eax
		mov dx, [ebp + 8]
		in al, dx
		pop ebp
		ret

global outb:function
outb:		;(short port, char value)
		push ebp
		mov ebp, esp
		mov ax, [ebp + 8]
		mov dx, [ebp + 12]
		out dx, ax
		pop ebp
		ret

global initPICS:function
initPICS:	;(void) returns void
		push ebp
		mov ebp, esp
		sub esp, 8

		;save masks
		in al, PIC1_DATA
		mov [ss:bp-4], eax
		in al, PIC2_DATA
		mov [ss:bp-8], eax

		;ICW1
		mov al, 0x11
		out PIC1_COMMAND, al
		call pic_wait
		out PIC2_COMMAND, al
		call pic_wait

		;ICW2
		mov al, 0x20 ;set pic1 to 32
		out PIC1_DATA, al
		call pic_wait
		mov al, 0x28 ;set pic2 to 40
		out PIC2_DATA, al
		call pic_wait

		;ICW3
		mov al, 0x04 ;tell master PIC that there is a second pic at IRQ 2
		out PIC1_DATA, al
		call pic_wait
		mov al, 0x02 ;slave id
		out PIC2_DATA, al
		call pic_wait

		;ICW4
		mov al, 0x01 ;8086 mode
		out PIC1_DATA, al
		call pic_wait
		out PIC2_DATA, al
		call pic_wait

		;restore masks
		mov al, [ss:bp-4]
		out PIC1_DATA, al
		mov al, [ss:bp-8]
		out PIC2_DATA, al

		mov esp, ebp
		pop ebp
		ret

pic_wait:	nop
		nop
		ret
