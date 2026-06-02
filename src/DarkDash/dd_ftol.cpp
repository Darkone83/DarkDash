/*---------------------------------------------------------------------------
    dd_ftol.cpp -- supply __ftol2_sse, which the RXDK CRT does not provide.

    minimp3's scalar synthesis performs float->int conversions, and the modern
    MSVC frontend emits calls to the CRT helper __ftol2_sse for them. That
    symbol is absent here (LNK2001), so we provide it.

    Convention: the floating-point value arrives on the x87 stack in st(0).
    We convert it to a 64-bit integer and return it in edx:eax, which serves
    both (int) callers (read eax) and (long long) callers (read edx:eax).

    Source name '_ftol2_sse' (extern "C", __cdecl) decorates to '__ftol2_sse',
    matching the unresolved symbol. x86-only (Xbox is 32-bit x86).
    One asm instruction per line, per the toolchain's preference.
---------------------------------------------------------------------------*/

#pragma warning(disable : 4731)

extern "C" __declspec(naked) void __cdecl _ftol2_sse(void)
{
    __asm
    {
        push    ebp
        mov     ebp, esp
        sub     esp, 4
        fistp   dword ptr[ebp - 4]
        mov     eax, dword ptr[ebp - 4]
        mov     esp, ebp
        pop     ebp
        ret
    }
}

#pragma warning( default : 4731 )