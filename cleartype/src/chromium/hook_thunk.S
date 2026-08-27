// The slot holds SkScalerContext::generateImage(const SkGlyph&, void*), so
// on entry rdi is the scaler context, rsi the glyph and rdx the image buffer.
// Those three registers reach the handler untouched, which is why it can
// declare them as its own parameters.
//
// The handler returns true when it has filled the image buffer itself, and
// this then returns without ever reaching Skia's own generateImage. False
// means it declined, and the original runs as if nothing were here - which
// is the path taken for every glyph DirectWrite cannot be asked for, and for
// every glyph at all when the DirectWrite backend is switched off.
//
// generateImage returns void, so nothing has to be produced in rax on the
// handled path.
//
// Entry RSP is 16n+8 as the SysV ABI leaves it at a call. The eight pushes
// keep that parity, and the explicit pad restores 16-byte alignment only
// around the handler call; everything is undone before the tail jump so the
// real callee sees exactly the frame a direct call would have given it,
// which keeps stack unwinding correct through this.

.text
.globl chromium_hook_thunk
.hidden chromium_hook_thunk
.type chromium_hook_thunk, @function
chromium_hook_thunk:
    .cfi_startproc
    push %rdi
    .cfi_adjust_cfa_offset 8
    push %rsi
    .cfi_adjust_cfa_offset 8
    push %rdx
    .cfi_adjust_cfa_offset 8
    push %rcx
    .cfi_adjust_cfa_offset 8
    push %r8
    .cfi_adjust_cfa_offset 8
    push %r9
    .cfi_adjust_cfa_offset 8
    push %r10
    .cfi_adjust_cfa_offset 8
    push %r11
    .cfi_adjust_cfa_offset 8
    sub $8, %rsp
    .cfi_adjust_cfa_offset 8
    call OnChromiumGenerateImage@PLT
    add $8, %rsp
    .cfi_adjust_cfa_offset -8
    test %al, %al
    pop %r11
    .cfi_adjust_cfa_offset -8
    pop %r10
    .cfi_adjust_cfa_offset -8
    pop %r9
    .cfi_adjust_cfa_offset -8
    pop %r8
    .cfi_adjust_cfa_offset -8
    pop %rcx
    .cfi_adjust_cfa_offset -8
    pop %rdx
    .cfi_adjust_cfa_offset -8
    pop %rsi
    .cfi_adjust_cfa_offset -8
    pop %rdi
    .cfi_adjust_cfa_offset -8
    jne .Lhandled
    movq g_original(%rip), %rax
    jmp *%rax
.Lhandled:
    ret
    .cfi_endproc
.size chromium_hook_thunk, . - chromium_hook_thunk

// SkTypeface::onFilterRec(SkScalerContextRec*) const: rdi is the typeface,
// rsi the rec. The rec has to be seen both before and after the real filter
// runs - before, because Fontations clears the flag that says the surface
// refused subpixel text, and after, because everything else it does should
// stand and only then be overridden.
//
// rsi is preserved across the first probe by keeping it on the stack, since
// the real onFilterRec is free to clobber it.

.text
.globl chromium_filter_rec_thunk
.hidden chromium_filter_rec_thunk
.type chromium_filter_rec_thunk, @function
chromium_filter_rec_thunk:
    .cfi_startproc
    push %rbp
    .cfi_adjust_cfa_offset 8
    mov %rsp, %rbp
    .cfi_def_cfa_register %rbp
    push %rdi
    .cfi_adjust_cfa_offset 8
    push %rsi
    .cfi_adjust_cfa_offset 8
    call OnChromiumFilterRec@PLT
    pop %rsi
    .cfi_adjust_cfa_offset -8
    pop %rdi
    .cfi_adjust_cfa_offset -8
    push %rsi
    .cfi_adjust_cfa_offset 8
    sub $8, %rsp
    .cfi_adjust_cfa_offset 8
    movq g_original_filter_rec(%rip), %rax
    call *%rax
    add $8, %rsp
    .cfi_adjust_cfa_offset -8
    pop %rdi
    .cfi_adjust_cfa_offset -8
    call OnChromiumFilterRecDone@PLT
    mov %rbp, %rsp
    pop %rbp
    .cfi_def_cfa_register %rsp
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
.size chromium_filter_rec_thunk, . - chromium_filter_rec_thunk

// SkScalerContext::generateMetrics(const SkGlyph&, SkArenaAlloc*) returns a
// GlyphMetrics of 64 bytes, so it is returned through a hidden pointer and
// every argument shifts along by one:
//
//   rdi  the buffer the result is written into
//   rsi  the scaler context
//   rdx  the glyph
//   rcx  the arena
//
// The original runs first and fills that buffer; the probe then sees the
// finished metrics and replaces the advance, which is the one field Windows
// measures differently. rax must come back holding the buffer, as the ABI
// requires of a function returning through memory.

.text
.globl chromium_metrics_thunk
.hidden chromium_metrics_thunk
.type chromium_metrics_thunk, @function
chromium_metrics_thunk:
    .cfi_startproc
    push %rbp
    .cfi_adjust_cfa_offset 8
    mov %rsp, %rbp
    .cfi_def_cfa_register %rbp
    push %rdi
    .cfi_adjust_cfa_offset 8
    push %rsi
    .cfi_adjust_cfa_offset 8
    push %rdx
    .cfi_adjust_cfa_offset 8
    sub $8, %rsp
    .cfi_adjust_cfa_offset 8
    movq g_original_metrics(%rip), %rax
    call *%rax
    add $8, %rsp
    .cfi_adjust_cfa_offset -8
    pop %rdx
    .cfi_adjust_cfa_offset -8
    pop %rsi
    .cfi_adjust_cfa_offset -8
    pop %rdi
    .cfi_adjust_cfa_offset -8
    // Sixteen bytes rather than a push, which would leave the stack
    // misaligned and fault on the callee's first SSE spill.
    sub $16, %rsp
    .cfi_adjust_cfa_offset 16
    mov %rdi, (%rsp)
    call OnChromiumMetrics@PLT
    mov (%rsp), %rax
    add $16, %rsp
    .cfi_adjust_cfa_offset -16
    mov %rbp, %rsp
    pop %rbp
    .cfi_def_cfa_register %rsp
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
.size chromium_metrics_thunk, . - chromium_metrics_thunk

// SkScalerContext::generateFontMetrics(SkFontMetrics*): rdi the scaler
// context, rsi the struct to fill. Returns void and takes no hidden pointer,
// so this is the simplest of the three - run the original, then let the probe
// overwrite the fields Windows computes differently.

.text
.globl chromium_font_metrics_thunk
.hidden chromium_font_metrics_thunk
.type chromium_font_metrics_thunk, @function
chromium_font_metrics_thunk:
    .cfi_startproc
    push %rbp
    .cfi_adjust_cfa_offset 8
    mov %rsp, %rbp
    .cfi_def_cfa_register %rbp
    push %rdi
    .cfi_adjust_cfa_offset 8
    push %rsi
    .cfi_adjust_cfa_offset 8
    movq g_original_font_metrics(%rip), %rax
    call *%rax
    pop %rsi
    .cfi_adjust_cfa_offset -8
    pop %rdi
    .cfi_adjust_cfa_offset -8
    // No padding. The two pops above already left the stack 16-byte aligned,
    // and adding any here would fault on the callee's first SSE spill.
    call OnChromiumFontMetrics@PLT
    mov %rbp, %rsp
    pop %rbp
    .cfi_def_cfa_register %rsp
    .cfi_adjust_cfa_offset -8
    ret
    .cfi_endproc
.size chromium_font_metrics_thunk, . - chromium_font_metrics_thunk

.section .note.GNU-stack,"",@progbits
