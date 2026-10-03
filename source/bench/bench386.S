/*
 * WiNspire x86 emulation benchmark payload.
 *
 * A real-mode boot sector program with a fixed instruction mix. It is booted
 * by the emulated BIOS so it exercises the full pipeline (BIOS POST, real-mode
 * decode, memory, device I/O) and then runs eight measured phases covering the
 * instruction classes that dominate a Windows 95 boot.
 *
 * Phase reporting: SETPHASE publishes the current phase number to the word at
 * linear address 0x50000 through segment 0x5000. The harness samples that word
 * and records the instruction count at each transition, which yields a
 * per-phase MIPS figure without needing any emulator-side hooks. 0xFF means
 * "finished".
 *
 * Two constraints shaped this layout:
 *   - 0x50000 is used rather than something in the first 64 KiB because the
 *     BIOS (SeaBIOS) freely scratches on low conventional memory after POST.
 *   - A 16-bit real-mode store cannot reach 0x50000 through a zero-based
 *     segment, so ES is parked at 0x5000 for the whole run and only phase 4
 *     (whose REP string ops need ES=0) switches it away.
 *   - Real mode defaults to 16-bit address size, so every memory operand that
 *     must reach above 64 KiB carries an explicit addr32 prefix. Without it
 *     the buffers below would silently address DI/SI truncated to 16 bits and
 *     overwrite the interrupt vector table.
 *
 * The mix is deliberately deterministic so repeated runs are comparable.
 *
 * Build: source/build-scripts/build_bench.sh
 */
	.code16
	.section .text

	.set MARKER_SEG, 0x5000		/* linear 0x50000 */
	.set SCRATCH,    0x60000	/* 64 KiB scratch buffer */
	.set N_ALU,      400000
	.set N_MOV,      400000
	.set N_MEM,      200000
	.set N_STACK,    150000
	.set N_REP,      4000
	.set N_SHIFT,    200000
	.set N_BRANCH,   300000
	.set N_BYTE,     200000

/* Publish a phase number. ES must already be MARKER_SEG. */
	.macro	SETPHASE n
	movw	$\n, %es:0
	.endm

/* Switch the data-segment-for-strings (ES) between the marker and guest RAM. */
	.macro	ES_MARKER
	movw	$MARKER_SEG, %ax
	movw	%ax, %es
	.endm

	.macro	ES_RAM
	xorw	%ax, %ax
	movw	%ax, %es
	.endm

	.globl _start
_start:
	cli
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss
	movw	$0x7c00, %sp
	cld
	ES_MARKER
	sti

	/* ---------------- phase 0: 32-bit ALU chain ---------------- */
	SETPHASE 0
	movl	$N_ALU, %esi
	movl	$0x12345678, %eax
	movl	$0x0f0f0f0f, %ebx
	xorl	%ecx, %ecx
	xorl	%edx, %edx
1:
	addl	%ebx, %eax
	xorl	%eax, %edx
	subl	%ebx, %ecx
	orl	%edx, %eax
	andl	%ebx, %ecx
	cmpl	%eax, %ebx
	adcl	$1, %edx
	decl	%esi
	jnz	1b

	/* ---------------- phase 1: register moves --------------- */
	SETPHASE 1
	movl	$N_MOV, %esi
	movl	$0x12345678, %eax
2:
	movl	%eax, %edi
	movl	%edi, %ebp
	movl	%ebp, %ebx
	movl	%ebx, %ecx
	movw	%cx, %dx
	movzbl	%dh, %ebx
	movswl	%bx, %ecx
	movl	%ecx, %eax
	decl	%esi
	jnz	2b

	/* ---------------- phase 2: memory load/store ---------------- */
	SETPHASE 2
	movl	$N_MEM, %esi
	movl	$SCRATCH, %edi
	movl	$0xaaaaaaaa, %eax
3:
	addr32 movl	%eax, (%edi)
	addr32 movl	(%edi), %ebx
	addl	%ebx, %eax
	addr32 movl	%eax, 4(%edi)
	addr32 movl	4(%edi), %ecx
	xorl	%ecx, %eax
	addr32 movl	%eax, (%edi)
	addr32 movw	(%edi), %dx
	addl	%esi, %edi
	andl	$0x3fff, %edi
	orl	$SCRATCH, %edi
	decl	%esi
	jnz	3b

	/* ---------------- phase 3: push/pop/call/ret --------------- */
	SETPHASE 3
	movl	$N_STACK, %esi
	movl	$0x11111111, %eax
4:
	pushl	%eax
	pushl	%ebx
	pushl	%ecx
	popl	%edx
	popl	%ebp
	popl	%edi
	/* call/ret round trip; the helper sits after the loop. */
	call	20f
	decl	%esi
	jnz	4b
	jmp	22f
20:
	addl	$7, %eax
	xorl	%ebp, %edx
	ret
22:

	/* ---------------- phase 4: REP string ops ------------------ */
	SETPHASE 4
	/* REP stores go to ES:DI, so ES has to point at guest RAM here. */
	ES_RAM
	/* REP uses ecx/esi/edi, so the outer counter lives in ebp. */
	movl	$N_REP, %ebp
6:
	movl	$SCRATCH, %edi
	movl	$0x11111111, %eax
	movl	$2048, %ecx
	addr32 rep stosl
	movl	$SCRATCH, %esi
	movl	$SCRATCH + 0x8000, %edi
	movl	$2048, %ecx
	addr32 rep movsl
	movl	$SCRATCH + 0x8000, %edi
	movl	$2048, %ecx
	addr32 rep stosb
	decl	%ebp
	jnz	6b
	ES_MARKER

	/* ---------------- phase 5: shifts and multiplies ----------- */
	SETPHASE 5
	movl	$N_SHIFT, %esi
	movl	$0x0badf00d, %eax
7:
	shll	$3, %eax
	shrl	$2, %eax
	sarl	$1, %eax
	roll	$7, %eax
	rorl	$5, %eax
	imull	$0x10001, %eax, %ebx
	mull	%ebx
	xorl	%edx, %eax
	movl	%eax, %ebx
	decl	%esi
	jnz	7b

	/* ---------------- phase 6: branches ------------------------ */
	SETPHASE 6
	movl	$N_BRANCH, %esi
	xorl	%eax, %eax
	xorl	%edx, %edx
8:
	incl	%eax
	movl	%eax, %ecx
	andl	$1, %ecx
	jz	9f
	addl	$3, %edx
	jmp	10f
9:
	subl	$1, %edx
10:
	testl	%eax, %eax
	jnz	11f
	decl	%esi
	jmp	12f
11:
	decl	%esi
12:
	jnz	8b

	/* ---------------- phase 7: 8-bit / 16-bit ops ------------- */
	SETPHASE 7
	movl	$N_BYTE, %esi
	xorl	%eax, %eax
	movl	$SCRATCH, %edi
13:
	movb	%al, %bl
	movb	%bl, %cl
	addb	%cl, %al
	rolb	$3, %bl
	addr32 movb	%bl, (%edi)
	addr32 movb	(%edi), %dl
	xorb	%dl, %al
	incw	%di
	movw	%si, %bp
	decl	%esi
	jnz	13b

	/* ---------------- done ------------------------------------ */
	SETPHASE 0xff
	cli
14:
	hlt
	jmp	14b

	.org	510
	.word	0xaa55
