import struct, sys, glob, os
OPS = "MOVE LOADK LOADBOOL LOADNIL GETUPVAL GETGLOBAL GETTABLE SETGLOBAL SETUPVAL SETTABLE NEWTABLE SELF ADD SUB MUL DIV POW UNM NOT CONCAT JMP EQ LT LE TEST CALL TAILCALL RETURN FORLOOP TFORLOOP TFORPREP SETLIST SETLISTO CLOSE CLOSURE".split()
class R:
    def __init__(s, d): s.d = d; s.p = 0
    def b(s): v = s.d[s.p]; s.p += 1; return v
    def i(s): v = struct.unpack_from('<i', s.d, s.p)[0]; s.p += 4; return v
    def f(s): v = struct.unpack_from('<f', s.d, s.p)[0]; s.p += 4; return v
    def str(s):
        n = s.i()
        if n == 0: return None
        v = s.d[s.p:s.p + n - 1].decode('latin1'); s.p += n; return v
def func(r, depth=0):
    src = r.str(); line = r.i(); nups = r.b(); npar = r.b(); va = r.b(); ms = r.b()
    n = r.i(); r.p += 4 * n
    n = r.i()
    locs = []
    for _ in range(n): locs.append(r.str()); r.i(); r.i()
    n = r.i(); ups = [r.str() for _ in range(n)]
    n = r.i(); K = []
    for _ in range(n):
        t = r.b()
        if t == 3: K.append(r.f())
        elif t == 4: K.append(repr(r.str()))
        elif t == 0: K.append('nil')
        elif t == 1: K.append(bool(r.b()))
        else: raise Exception('const type %d' % t)
    n = r.i(); P = [func(r, depth + 1) for _ in range(n)]
    n = r.i(); code = [struct.unpack_from('<I', r.d, r.p + 4 * k)[0] for k in range(n)]; r.p += 4 * n
    return dict(src=src, K=K, P=P, code=code, npar=npar, ups=ups, locs=locs)
def rk(K, x): return ('K' + str(K[x - 250])) if x >= 250 else 'R%d' % x
def show(fn, ind=''):
    K = fn['K']
    print(ind + 'function %s params=%d ups=%s locals=%s' % (fn['src'], fn['npar'], fn['ups'], fn['locs']))
    for pc, ins in enumerate(fn['code']):
        op = ins & 0x3f; c = (ins >> 6) & 0x1ff; b = (ins >> 15) & 0x1ff; a = (ins >> 24) & 0xff; bx = (ins >> 6) & 0x3ffff; sbx = bx - 131071
        o = OPS[op] if op < len(OPS) else '?%d' % op
        if o in ('LOADK', 'GETGLOBAL', 'SETGLOBAL'): t = 'R%d %s' % (a, K[bx])
        elif o in ('GETTABLE', 'SELF'): t = 'R%d R%d[%s]' % (a, b, rk(K, c))
        elif o == 'SETTABLE': t = 'R%d[%s] = %s' % (a, rk(K, b), rk(K, c))
        elif o in ('EQ', 'LT', 'LE'): t = 'if (%s %s %s) ~= %d then pc++' % (rk(K, b), o, rk(K, c), a)
        elif o in ('ADD', 'SUB', 'MUL', 'DIV', 'POW'): t = 'R%d = %s %s %s' % (a, rk(K, b), o, rk(K, c))
        elif o == 'JMP': t = '-> %d' % (pc + 1 + sbx)
        elif o in ('FORLOOP', 'TFORPREP'): t = 'R%d -> %d' % (a, pc + 1 + sbx)
        elif o == 'CALL': t = 'R%d(args %d) results %d' % (a, b - 1, c - 1)
        elif o == 'TEST': t = 'R%d = R%d if R%d <=> %d else pc++' % (a, b, b, c)
        elif o == 'CLOSURE': t = 'R%d proto %d' % (a, bx)
        else: t = 'A%d B%d C%d' % (a, b, c)
        print(ind + '  %3d %-9s %s' % (pc, o, t))
    for p in fn['P']: show(p, ind + '    ')
def load(path):
    d = open(path, 'rb').read()
    r = R(d); r.p = 4 + 1 + 1 + 4 + 4 + 4   # sig ver end int size_t instr | sizes(4) | lua_Number size + test
    r.p = 14 + 4
    return func(r)
if __name__ == '__main__':
    for path in sys.argv[1:]:
        try: show(load(path))
        except Exception as e: print('ERR', path, e)
