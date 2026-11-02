"""Execute this engine's emitted AArch64 blocks with Unicorn; no Android/device installation.
Build tools/arm64_validation/bridge.cpp plus cpu_arm64.cpp with __aarch64__ defined.
On Windows add tools/arm64_validation/include (emit-only mmap adapters).
Requires unicorn==2.1.4. The bridge and geometry reference use this repository's sources.
"""
import argparse, ctypes as C, json, random, struct
from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm64_const import *
class State(C.Structure):
    _fields_=[('r',C.c_int32*32),('cop0',C.c_int32*32),('pc',C.c_uint32),('next',C.c_uint32),('hi',C.c_int32),('lo',C.c_int32),('loadReg',C.c_uint32),('loadValue',C.c_int32),('branchPending',C.c_uint32),('branchTaken',C.c_uint32),('faultBranchTaken',C.c_uint32),('faultBranchTarget',C.c_uint32),('lastException',C.c_uint32),('faultPc',C.c_uint32),('faultInstruction',C.c_uint32),('cacheIsolated',C.c_uint32),('instructions',C.c_uint64),('exceptions',C.c_uint64),('gte',C.c_int32*64)]
assert C.sizeof(State)==584
U=lambda n:n&0xffffffff
I=lambda n:C.c_int32(n).value
parser=argparse.ArgumentParser();parser.add_argument('--bridge',required=True);parser.add_argument('--cases',type=int,default=2000);parser.add_argument('--seed',type=int,default=0x505358);args=parser.parse_args()
dll=C.CDLL(args.bridge);dll.emit_block.argtypes=[C.POINTER(C.c_uint32),C.c_int,C.c_uint32,C.POINTER(C.c_uint8),C.c_int];dll.gte_address.restype=C.c_size_t;dll.run_gte.argtypes=[C.POINTER(State),C.c_uint32,C.c_int,C.c_int];dll.prefix_limit.argtypes=[C.POINTER(C.c_uint32),C.c_int,C.c_int,C.c_int,C.c_int];assert dll.register_counter_check()==0
CODE,STATE,RAM,GEN,MASK,STACK,STOP=0x100000,0x10000000,0x20000000,0x30000000,0x31000000,0x40000000,0x50000000
helper=dll.gte_address();rng=random.Random(args.seed);stats={'cases':0,'instructions':0,'gte_helpers':0,'prefix_exits':0,'register_counters':'passed'}
def reference(s,w,ram,gen,mask,scale):
    op=w>>26;rs=w>>21&31;rt=w>>16&31;rd=w>>11&31;fun=w&63;shift=w>>6&31;imm=C.c_int16(w&65535).value
    a,b=s.r[rs],s.r[rt];at=s.pc;s.faultBranchTaken=s.branchTaken;s.faultBranchTarget=s.next
    if s.loadReg:s.r[s.loadReg]=s.loadValue
    s.loadReg=0;s.pc=s.next;s.next=U(s.next+4);s.branchPending=s.branchTaken=0
    def put(r,v):
        if r:s.r[r]=I(v)
    if op==0:
        if fun in [8,9]:
            if fun==9:put(rd,at+8)
            s.next=U(a);s.branchPending=s.branchTaken=1
        elif fun in [16,18]:put(rd,s.hi if fun==16 else s.lo)
        elif fun in [17,19]:setattr(s,'hi' if fun==17 else 'lo',a)
        elif fun in [24,25]:
            product=a*b if fun==24 else U(a)*U(b);s.lo=I(product);s.hi=I(product>>32)
        elif fun in [26,27]:
            if not b:q=(-1 if a>=0 else 1) if fun==26 else -1;r=a
            elif fun==26:q=(abs(a)//abs(b))*(-1 if (a<0)!=(b<0) else 1);r=a-q*b
            else:q=U(a)//U(b);r=U(a)%U(b)
            s.lo=I(q);s.hi=I(r)
        else:
            v={0:lambda:U(b)<<shift,2:lambda:U(b)>>shift,3:lambda:b>>shift,4:lambda:U(b)<<(a&31),6:lambda:U(b)>>(a&31),7:lambda:b>>(a&31),33:lambda:a+b,35:lambda:a-b,36:lambda:a&b,37:lambda:a|b,38:lambda:a^b,39:lambda:~(a|b),42:lambda:int(a<b),43:lambda:int(U(a)<U(b))}[fun]();put(rd,v)
    elif 1<=op<=7:
        if op in [2,3]:taken=True;target=((at+4)&0xf0000000)|((w&0x3ffffff)<<2)
        else:
            taken=({1:lambda:(a>=0 if rt&1 else a<0),4:lambda:a==b,5:lambda:a!=b,6:lambda:a<=0,7:lambda:a>0}[op])();target=U(at+4+imm*4)
        if op==3 or op==1 and rt in [16,17]:put(31,at+8)
        s.branchPending=1;s.branchTaken=int(taken)
        if taken:s.next=target
    elif op==18:
        dll.run_gte(C.byref(s),w,scale,b)
        if rs in [0,2]:s.loadReg=rt
    elif op>=32:
        address=U(a+imm)&0x1fffff;width=1 if op in [32,36,40] else 2 if op in [33,37,41] else 4
        if op in [40,41,43]:
            new=U(b).to_bytes(4,'little')[:width];old=ram[address:address+width];ram[address:address+width]=new
            if old!=new and mask[address>>5]&(1<<((address>>2)&7)):gen[address>>12]+=1
        else:
            value=int.from_bytes(ram[address:address+width],'little',signed=op in [32,33]);s.loadReg=rt;s.loadValue=I(value)
    else:put(rt,{9:lambda:a+imm,10:lambda:int(a<imm),11:lambda:int(U(a)<U(imm)),12:lambda:a&(w&65535),13:lambda:a|(w&65535),14:lambda:a^(w&65535),15:lambda:(w&65535)<<16}[op]())
    s.r[0]=0

def check(words,initial,budget,scale=65536,code_store=False):
    data=(C.c_uint32*len(words))(*words);out=(C.c_uint8*32768)();length=dll.emit_block(data,len(words),initial.pc,out,len(out));assert length>0,('not compiled',words)
    uc=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
    for address,size in [(CODE,32768),(STATE,4096),(RAM,2*1024*1024),(GEN,8192),(MASK,65536),(STACK,65536),(STOP,4096)]:uc.mem_map(address,size)
    uc.mem_map(helper&~4095,4096);uc.mem_write(helper,b'\xc0\x03\x5f\xd6')
    ram=bytearray(2*1024*1024);ram[0x50000:0x50400]=rng.randbytes(1024);ram[0x10000:0x10000+len(words)*4]=struct.pack('<'+'I'*len(words),*words)
    gen=[0]*512;mask=bytearray(65536)
    if code_store:
        for off in range(0x10000,0x10000+len(words)*4,4):mask[off>>5]|=1<<((off>>2)&7)
    uc.mem_write(CODE,bytes(out[:length]));uc.mem_write(STATE,bytes(initial));uc.mem_write(RAM,bytes(ram));uc.mem_write(MASK,bytes(mask))
    for reg,value in [(UC_ARM64_REG_X0,STATE),(UC_ARM64_REG_X1,RAM),(UC_ARM64_REG_X2,GEN),(UC_ARM64_REG_X3,MASK),(UC_ARM64_REG_X4,budget),(UC_ARM64_REG_X5,scale),(UC_ARM64_REG_SP,STACK+65520),(UC_ARM64_REG_LR,STOP)]:uc.reg_write(reg,value)
    def hook(u,address,size,unused):
        if address!=helper:return
        ptr=u.reg_read(UC_ARM64_REG_X0);s=State.from_buffer_copy(u.mem_read(ptr,584));word=u.reg_read(UC_ARM64_REG_W1);scale=u.reg_read(UC_ARM64_REG_W2);value=I(u.reg_read(UC_ARM64_REG_W3));ret=u.reg_read(UC_ARM64_REG_LR)
        dll.run_gte(C.byref(s),word,scale,value);u.mem_write(ptr,bytes(s));stats['gte_helpers']+=1
        # A real C++ leaf is free to destroy all caller-owned registers and flags.
        for reg in range(UC_ARM64_REG_X0,UC_ARM64_REG_X18+1):u.reg_write(reg,0xBAD00000+reg)
        u.reg_write(UC_ARM64_REG_NZCV,0xf0000000);u.reg_write(UC_ARM64_REG_PC,ret)
    for reg in range(UC_ARM64_REG_X19,UC_ARM64_REG_X28+1):uc.reg_write(reg,0xCA110000+reg)
    uc.hook_add(UC_HOOK_CODE,hook,begin=helper,end=helper)
    try:uc.emu_start(CODE,STOP,count=20000)
    except Exception as e:raise AssertionError((str(e),[hex(w) for w in words],budget)) from e
    assert uc.reg_read(UC_ARM64_REG_SP)==STACK+65520,"Stack pointer changed"
    for reg in range(UC_ARM64_REG_X19,UC_ARM64_REG_X28+1):assert uc.reg_read(reg)==0xCA110000+reg,"Callee-saved register changed"
    count=uc.reg_read(UC_ARM64_REG_X0);assert count<=min(len(words),budget),(count,budget)
    actual=bytes(uc.mem_read(STATE,584));expected=State.from_buffer_copy(bytes(initial))
    for word in words[:count]:reference(expected,word,ram,gen,mask,scale)
    if actual!=bytes(expected):
        bad=[i for i,(a,b) in enumerate(zip(actual,bytes(expected))) if a!=b]
        raise AssertionError({'words':[hex(w) for w in words],'count':count,'budget':budget,'bytes':bad[:25],'actual_words':struct.unpack('<'+'I'*82,actual[:328]),'expected_words':struct.unpack('<'+'I'*82,bytes(expected)[:328])})
    assert bytes(uc.mem_read(RAM,len(ram)))==ram,'RAM differs'
    assert bytes(uc.mem_read(GEN,4096))==struct.pack('<'+'Q'*512,*gen),'code generations differ'
    stats['cases']+=1;stats['instructions']+=count;stats['prefix_exits']+=int(count<len(words))

def state():
    s=State();s.pc=0x80010000;s.next=s.pc+4;s.faultBranchTarget=s.pc;s.instructions=123;s.cop0[12]=0x40000000
    for i in range(1,32):s.r[i]=I(rng.getrandbits(32))
    s.r[20]=0x80050000;s.hi=I(rng.getrandbits(32));s.lo=I(rng.getrandbits(32))
    s.loadReg=rng.randrange(0,20);s.loadValue=I(rng.getrandbits(32))
    for i in range(64):s.gte[i]=I(rng.getrandbits(32))
    return s
special=[0,2,3,4,6,7,16,17,18,19,24,25,26,27,33,35,36,37,38,39,42,43];primary=[9,10,11,12,13,14,15,32,33,35,36,37]
gte_ops=[1,6,0xc,0x10,0x11,0x12,0x13,0x14,0x16,0x1b,0x1c,0x1e,0x20,0x28,0x29,0x2a,0x2d,0x2e,0x30,0x3d,0x3e,0x3f]
for index in range(args.cases):
    s=state();words=[];n=rng.randint(1,64)
    for i in range(n):
        rs=rng.randrange(20);rt=rng.randrange(20);rd=rng.randrange(20)
        if rng.randrange(6)==0:
            mode=rng.choice([0,2,4,6,16]);word=18<<26|mode<<21|rt<<16|rd<<11
            if mode==16:word|=rng.choice(gte_ops)|rng.choice([0,0x80000,0x80400])
        elif rng.randrange(2):word=rs<<21|rt<<16|rd<<11|rng.randrange(32)<<6|rng.choice(special)
        else:
            op=rng.choice(primary);imm=rng.randrange(256)
            if op>=32:rs=20;imm&=~(3 if op==35 else 1 if op in [33,37] else 0)
            word=op<<26|rs<<21|rt<<16|imm
        words.append(word)
    if n<63 and index%3==0:
        op=rng.choice([1,2,3,4,5,6,7,0]);rs=rng.randrange(20);rt=rng.choice([0,1,16,17]) if op==1 else rng.randrange(20)
        branch=(rs<<21|31<<11|rng.choice([8,9])) if op==0 else (op<<26|0x40020) if op in [2,3] else op<<26|rs<<21|rt<<16|rng.randrange(8)
        words.extend([branch,0x24210001 if index%2 else 0x00220018])
    check(words,s,rng.randrange(len(words)+1),rng.choice([49152,65536,73728]))
# Defined division corner cases and delay-slot writes after pending loads.
for op in [24,25,26,27]:
 for a in [0,1,-1,-2147483648,2147483647]:
  for b in [0,1,-1,-2147483648,2147483647]:
   s=state();s.loadReg=0;s.r[1]=a;s.r[2]=b;check([1<<21|2<<16|op,3<<11|16,4<<11|18],s,3)
for op in [40,41,43]:
 for changed in [False,True]:
  s=state();s.loadReg=0;s.r[20]=0x80010000;s.r[1]=I((op<<26|20<<21|1<<16)^int(changed));check([op<<26|20<<21|1<<16],s,1,code_store=True)
# Guard exits must leave the pending load available for the interpreter slow path.
for address in [0x1f801814,0x1f800000,0x00800000,0x80050001,0x80050002]:
    s=state();s.r[20]=I(address);check([0x24010007,0x8e820000,0x24430001],s,3)
for mode in [0,2,4,6,16]:
    s=state();s.cop0[12]=0;check([0x24010007,18<<26|mode<<21|2<<16|1],s,2)
# A delayed load must not change an immediately following branch's old operand.
for op in [0,1,2,3,4,5,6,7]:
    for variant in [0,1,2]:
        s=state();s.r[1]=[-1,0,1][variant];s.loadReg=1;s.loadValue=1-s.r[1]
        branch=(1<<21|1<<11|9) if op==0 else (op<<26|0x40020) if op in [2,3] else op<<26|1<<21|1<<16|4
        check([branch,0x24210003],s,2)
for words in [[12],[0x00221820],[0x49000000],[0xc8000000],[0x10220001,0x8c030000],[0x10220001,0x4a000006]]:
    data=(C.c_uint32*len(words))(*words);out=(C.c_uint8*32768)();assert dll.emit_block(data,len(words),0x80010000,out,len(out))==-1,('unsupported block accepted',words)
for _ in range(5000):
    words=[rng.choice([0x24420001,0x8c220000,0x80220000,0xac220000,0x4a000006]) for i in range(rng.randint(1,64))]
    clocks=rng.randrange(2000);fetch=rng.choice([0,6,24]);wait=rng.randrange(2);total=0;expected=0
    for w in words:
        cost=1+fetch+(6 if wait and w>>26 in [32,35] else 0)
        if total+cost>clocks:break
        total+=cost;expected+=1
    data=(C.c_uint32*len(words))(*words);assert dll.prefix_limit(data,len(words),clocks,fetch,wait)==expected
stats['clock_prefix_checks']=5000
print(json.dumps(stats,sort_keys=True))
