/*---------------------------------------------------------------------------
    dd_ftol.cpp -- supply __ftol2_sse, which the RXDK CRT does not provide.

    minimp3's scalar synthesis (and any other float->int cast anywhere in the
    program, since we build /GL) makes the MSVC frontend emit calls to the CRT
    helper __ftol2_sse. That symbol is absent here (LNK2001), so we provide it.

    Semantics that MUST match a C cast:
      * Convert TOWARD ZERO (truncate), not round-to-nearest. x87's default
        rounding is nearest, so we temporarily force RC=11b (chop) around the
        store, then restore the caller's control word. Without this, any value
        whose fraction is >= 0.5 comes back off by one -- an occasional,
        data-dependent integer bug.
      * Return the full 64-bit result in edx:eax. __ftol2_sse is the 64-bit
        helper; (int) callers read eax, (__int64) callers read edx:eax, and
        out-of-int32 magnitudes need the qword store to be meaningful.

    Convention: the value arrives on the x87 stack in st(0) (this MSVC2003
    frontend passes float->int args there even under the _sse helper name).

    Source name '_ftol2_sse' (extern "C", __cdecl) decorates to '__ftol2_sse'.
    x86-only (Xbox is 32-bit x86). One asm instruction per line.
---------------------------------------------------------------------------*/

#pragma warning(disable : 4731)

extern "C" __declspec(naked) void __cdecl _ftol2_sse(void)
{
    __asm
    {
        push    ebp
        mov     ebp, esp
        sub     esp, 16; [ebp - 2] = saved CW[ebp - 4] = chop CW[ebp - 12..ebp - 5] = qword
        fnstcw  word ptr[ebp - 2]; save caller's control word
        movzx   eax, word ptr[ebp - 2]
        or ah, 0Ch; RC field(bits 10 - 11) = 11b->round toward zero
        mov     word ptr[ebp - 4], ax
        fldcw   word ptr[ebp - 4]; install chop rounding
        fistp   qword ptr[ebp - 12]; st(0) -> 64 - bit int, truncated, pops st
        fldcw   word ptr[ebp - 2]; restore caller's control word
        mov     eax, dword ptr[ebp - 12]
        mov     edx, dword ptr[ebp - 8]
        mov     esp, ebp
        pop     ebp
        ret
    }
}

#pragma warning( default : 4731 )