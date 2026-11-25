"""mw2emu - run functions of the original DOS MW2.EXE (MechWarrior 2 1.1) (as the 32-bit ELF image Ghidra made of its LE executable) under
unicorn, for differential tests against the port's C (tools/audit/simdiff.py).

The image is NOT in the repo (game data): pass its path (env MW2_ELF, default mw2dos.elf).
Calling convention: Watcom register (EAX, EDX, EBX, ECX, then the stack right to left; the callee pops the stack
arguments). Callees can be stubbed (a Python function that sets EAX/EDX and returns), memory written by the call is
recorded, and any access outside the image / stack / test heap stops the run with an error (so a test sees when a
function's memory footprint is not what it assumed)."""
import os, struct
from unicorn import Uc, UcError, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE, UC_HOOK_MEM_INVALID
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EBX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_ESI,
                               UC_X86_REG_EDI, UC_X86_REG_EBP, UC_X86_REG_ESP, UC_X86_REG_EIP, UC_X86_REG_FPCW)

STACK_TOP = 0x00300000
STACK_BASE = 0x00200000
HEAP = 0x00400000          # test structures (bump allocator)
HEAP_SIZE = 0x00400000
RET_MAGIC = 0x00190000     # return address of the outermost call (a mapped page of hlt)

def s32(v):
    v &= 0xffffffff
    return v - (1 << 32) if v & 0x80000000 else v

class Emu:
    def __init__(self, path=None):
        path = path or os.environ.get('MW2_ELF', 'mw2dos.elf')
        self.img = open(path, 'rb').read()
        self.uc = Uc(UC_ARCH_X86, UC_MODE_32)
        self.segs = []
        phoff, phnum = struct.unpack_from('<I', self.img, 28)[0], struct.unpack_from('<H', self.img, 44)[0]
        for i in range(phnum):
            t, off, va, pa, fs, ms = struct.unpack_from('<6I', self.img, phoff + 32 * i)
            if t != 1: continue
            lo, hi = va & ~0xfff, (va + ms + 0xfff) & ~0xfff
            self.uc.mem_map(lo, hi - lo)
            self.uc.mem_write(va, self.img[off:off + fs])
            self.segs.append((va, va + ms, off))
        self.uc.mem_map(STACK_BASE, STACK_TOP - STACK_BASE)
        self.uc.mem_map(HEAP, HEAP_SIZE)
        self.uc.mem_map(RET_MAGIC, 0x1000)
        self.uc.mem_write(RET_MAGIC, b'\xf4' * 0x1000)
        self.err = None
        self.stubs = {}
        self.writes = []
        self.record = False
        self.heap_ptr = HEAP
        self.uc.hook_add(UC_HOOK_CODE, self._code)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self._wr)
        self.uc.hook_add(UC_HOOK_MEM_INVALID, self._bad)
        self.uc.reg_write(UC_X86_REG_FPCW, self.r16(0xa4520))  # Watcom startup at 0x78159
        # The sine and atan tables depend on the x87 precision in effect at startup.
        self.call(0x3aa90)
        self.pristine = {va: bytes(self.uc.mem_read(va, hi - va)) for va, hi, _ in self.segs if va >= 0x90000}

    # --- memory -------------------------------------------------------------------------------------------------
    def reset_data(self):
        """restore the image's data segment (between tests) and the heap pointer"""
        for va, b in self.pristine.items(): self.uc.mem_write(va, b)
        self.heap_ptr = HEAP
    def alloc(self, n, fill=0):
        a = self.heap_ptr
        self.heap_ptr = (self.heap_ptr + n + 15) & ~15
        self.uc.mem_write(a, bytes([fill]) * n)
        return a
    def r32(self, a): return struct.unpack('<I', self.uc.mem_read(a, 4))[0]
    def rs32(self, a): return struct.unpack('<i', self.uc.mem_read(a, 4))[0]
    def r16(self, a): return struct.unpack('<H', self.uc.mem_read(a, 2))[0]
    def rs16(self, a): return struct.unpack('<h', self.uc.mem_read(a, 2))[0]
    def r8(self, a): return self.uc.mem_read(a, 1)[0]
    def w32(self, a, v): self.uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
    def w16(self, a, v): self.uc.mem_write(a, struct.pack('<H', v & 0xffff))
    def w8(self, a, v): self.uc.mem_write(a, bytes([v & 0xff]))
    def read(self, a, n): return bytes(self.uc.mem_read(a, n))
    def write(self, a, b): self.uc.mem_write(a, b)

    # --- hooks --------------------------------------------------------------------------------------------------
    def stub(self, addr, fn=None, ret=0, pops=0):
        """replace the function at addr: fn(emu) -> eax (or (eax, edx)); pops = bytes of stack arguments to pop"""
        self.stubs[addr] = (fn, ret, pops)
    def _code(self, uc, addr, size, _):
        st = self.stubs.get(addr)
        if st is None: return
        fn, ret, pops = st
        r = fn(self) if fn else ret
        if isinstance(r, tuple): uc.reg_write(UC_X86_REG_EAX, r[0] & 0xffffffff); uc.reg_write(UC_X86_REG_EDX, r[1] & 0xffffffff)
        elif r is not None: uc.reg_write(UC_X86_REG_EAX, r & 0xffffffff)
        esp = uc.reg_read(UC_X86_REG_ESP)
        ra = self.r32(esp)
        uc.reg_write(UC_X86_REG_ESP, esp + 4 + pops)
        uc.reg_write(UC_X86_REG_EIP, ra)
    def _wr(self, uc, access, addr, size, value, _):
        if self.record: self.writes.append((addr, size, value))
    def _bad(self, uc, access, addr, size, value, _):
        self.err = 'bad access %#x size %d at eip %#x' % (addr, size, uc.reg_read(UC_X86_REG_EIP))
        return False

    def arg(self, n):
        """stack argument n (0 = the first one after the four registers) inside a stub"""
        return self.r32(self.uc.reg_read(UC_X86_REG_ESP) + 4 + 4 * n)
    def reg(self, r): return self.uc.reg_read({'eax': UC_X86_REG_EAX, 'ebx': UC_X86_REG_EBX, 'ecx': UC_X86_REG_ECX,
                                                'edx': UC_X86_REG_EDX, 'esi': UC_X86_REG_ESI, 'edi': UC_X86_REG_EDI}[r])

    # --- calls --------------------------------------------------------------------------------------------------
    def call(self, addr, eax=0, edx=0, ebx=0, ecx=0, stack=(), esi=0, edi=0, limit=5_000_000):
        uc = self.uc
        esp = STACK_TOP - 0x100
        for v in reversed(list(stack)):
            esp -= 4; self.w32(esp, v)
        esp -= 4; self.w32(esp, RET_MAGIC)
        for r, v in ((UC_X86_REG_EAX, eax), (UC_X86_REG_EDX, edx), (UC_X86_REG_EBX, ebx), (UC_X86_REG_ECX, ecx),
                     (UC_X86_REG_ESI, esi), (UC_X86_REG_EDI, edi), (UC_X86_REG_EBP, 0), (UC_X86_REG_ESP, esp)):
            uc.reg_write(r, v & 0xffffffff)
        self.writes = []
        self.err = None
        try:
            uc.emu_start(addr, RET_MAGIC, count=limit)
        except UcError as e:
            raise RuntimeError('%s: %s' % (e, self.err or 'eip %#x' % uc.reg_read(UC_X86_REG_EIP)))
        if uc.reg_read(UC_X86_REG_EIP) != RET_MAGIC:
            raise RuntimeError('did not return (eip %#x)' % uc.reg_read(UC_X86_REG_EIP))
        return s32(uc.reg_read(UC_X86_REG_EAX))
    def run(self, start, until, limit=1_000_000, **regs):
        """execute the code from start to the address until (a piece of a function: a branch tree on chosen register /
        memory values); regs: eax= ebx= ... esp defaults to the test stack, ebp too (frame-relative loads read the stack)"""
        names = {'eax': UC_X86_REG_EAX, 'ebx': UC_X86_REG_EBX, 'ecx': UC_X86_REG_ECX, 'edx': UC_X86_REG_EDX,
                 'esi': UC_X86_REG_ESI, 'edi': UC_X86_REG_EDI, 'ebp': UC_X86_REG_EBP, 'esp': UC_X86_REG_ESP}
        self.uc.reg_write(UC_X86_REG_ESP, STACK_TOP - 0x1000); self.uc.reg_write(UC_X86_REG_EBP, STACK_TOP - 0x800)
        for k, v in regs.items(): self.uc.reg_write(names[k], v & 0xffffffff)
        self.err = None
        try:
            self.uc.emu_start(start, until, count=limit)
        except UcError as e:
            raise RuntimeError('%s: %s' % (e, self.err or 'eip %#x' % self.uc.reg_read(UC_X86_REG_EIP)))
        if self.uc.reg_read(UC_X86_REG_EIP) != until:
            raise RuntimeError('did not reach %#x (eip %#x)' % (until, self.uc.reg_read(UC_X86_REG_EIP)))
    def edx(self): return s32(self.uc.reg_read(UC_X86_REG_EDX))
