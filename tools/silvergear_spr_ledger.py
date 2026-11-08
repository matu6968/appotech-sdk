#!/usr/bin/env python3
"""Static SPR-write ledger for the Silvergear stock application.

Walks the objdump listing of the stock app from one or more root functions with a
small constant-propagating interpreter and lists every `l.mtspr` it can resolve
(address, value or read-modify-write mask/bits), following direct calls. It is a
static aid, not an emulator: loads from memory are unknown, each instruction is
visited once per call, and unresolved branches are explored both ways (events
reached that way carry cond>0).

  silvergear_spr_ledger.py LISTING --root 0x2054330 [--root 0x2000000] [--diff dump.json]

--diff takes a tools/dump_sprs.py snapshot and reports, per SPR, the final ledger
write whose forced bits differ from the snapshot (candidates for what the SDK is
missing).
"""
import argparse,hashlib,json,re,struct,sys
from pathlib import Path
M32=0xffffffff
LINE=re.compile(r'^\s*([0-9a-f]+):\t(?:[0-9a-f]{2} ){4}\t(\S+)\s*(.*)$')
CALLEE_SAVED={1,2,14,16,18,20,22,24,26,28,30}
NO_DEST={'l.sw','l.sb','l.sh','l.mtspr','l.nop','l.j','l.jal','l.jr','l.jalr','l.bf','l.bnf','l.sys','l.trap','l.msync','l.psync','l.csync','l.rfe'}

class Val:
    """(spr & m) | b; base is the SPR read (None for a constant, then m == 0)."""
    __slots__=('base','m','b')
    def __init__(s,base,m,b): s.base,s.m,s.b=base,m&M32,b&M32
    @property
    def const(s): return s.base is None
def C(v): return Val(None,0,v&M32)

def signed(v): return v-(1<<32) if v&0x80000000 else v
def parse_imm(t):
    t=t.strip();return int(t,16) if t.lower().startswith(('0x','-0x')) else int(t)
def reg(t): return int(t.strip()[1:])

def load(path):
    code={}
    for line in Path(path).read_text().splitlines():
        m=LINE.match(line)
        if m: code[int(m.group(1),16)]=(m.group(2),[x.strip() for x in m.group(3).split(',')] if m.group(3) else [])
    return code

class Walker:
    def __init__(s,code,max_depth,trace_calls,literal_words=None):
        s.code,s.max_depth,s.trace=code,max_depth,set(trace_calls)
        s.literal_words=literal_words or {}
        s.events=[];s.calls=[];s.unresolved=set();s.active=set()
    def get(s,regs,n): return C(0) if n==0 else regs.get(n)
    def bin(s,op,a,b):
        if a is None or b is None: return None
        if op in('and','andi'):
            if a.const and b.const: return C(a.b&b.b)
            if b.const: return Val(a.base,a.m&b.b,a.b&b.b)
            if a.const: return Val(b.base,b.m&a.b,b.b&a.b)
            return None
        if op in('or','ori'):
            if a.const and b.const: return C(a.b|b.b)
            if b.const: return Val(a.base,a.m,a.b|b.b)
            if a.const: return Val(b.base,b.m,b.b|a.b)
            return None
        if not(a.const and b.const): return None
        x,y=a.b,b.b
        r={'add':x+y,'addi':x+y,'sub':x-y,'xor':x^y,'xori':x^y,'mul':x*y,'muli':x*y,
           'sll':x<<(y&31),'slli':x<<(y&31),'srl':x>>(y&31),'srli':x>>(y&31),
           'sra':signed(x)>>(y&31),'srai':signed(x)>>(y&31)}.get(op)
        if op in('div','divu') and y:
            if op=='div':
                sx,sy=signed(x),signed(y)
                r=(abs(sx)//abs(sy))*(-1 if (sx<0)!=(sy<0) else 1)
            else:r=x//y
        return None if r is None else C(r)
    def walk(s,fn,args,depth,cond0=0):
        if fn in s.active:
            s.unresolved.add((fn,fn,'recursive call'));return
        s.active.add(fn)
        visited=set();stack=[(fn,{n:v for n,v in args.items()},None,cond0)]
        while stack:
            pc,regs,flag,cond=stack.pop()
            while True:
                if pc in visited:break
                if pc not in s.code:
                    s.unresolved.add((fn,pc,'missing instruction'));break
                visited.add(pc)
                op,a=s.code[pc];nxt=pc+4
                if op=='l.jr':
                    if a[0]=='r9':break
                    target=s.get(regs,reg(a[0]))
                    if target is not None and target.const and target.b in s.code:
                        pc=target.b;continue
                    s.unresolved.add((fn,pc,'indirect jump: '+a[0]));break
                if op=='l.j': pc=parse_imm(a[0]);continue
                if op in('l.bf','l.bnf'):
                    t=parse_imm(a[0])
                    if flag is None:
                        stack.append((t,dict(regs),None,cond+1));cond+=1  # explore taken path later
                    else:
                        if flag==(op=='l.bf'): pc=t;continue
                    pc=nxt;continue
                if op=='l.jal' or op=='l.jalr':
                    target=parse_imm(a[0]) if op=='l.jal' else (s.get(regs,reg(a[0])).b if s.get(regs,reg(a[0])) and s.get(regs,reg(a[0])).const else None)
                    cargs={n:s.get(regs,n) for n in range(3,9)}
                    if target is not None:
                        if target in s.trace: s.calls.append((depth,fn,pc,target,cargs,cond))
                        if depth<s.max_depth: s.walk(target,cargs,depth+1,cond)
                        else:s.unresolved.add((fn,pc,'depth limit calling '+hex(target)))
                    else:s.unresolved.add((fn,pc,'indirect call: '+a[0]))
                    for n in list(regs):
                        if n not in CALLEE_SAVED: del regs[n]
                    flag=None  # Calls may change the condition flag as well as volatile GPRs.
                    pc=nxt;continue
                if op.startswith('l.sf'):
                    x=s.get(regs,reg(a[0]))
                    y=s.get(regs,reg(a[1])) if not op.endswith('i') else C(parse_imm(a[1])&M32)
                    flag=None
                    if x is not None and y is not None and x.const and y.const:
                        n=op[4:].rstrip('i') if op.endswith('i') else op[4:]
                        xs,ys=signed(x.b),signed(y.b)
                        flag={'eq':x.b==y.b,'ne':x.b!=y.b,'gtu':x.b>y.b,'geu':x.b>=y.b,'ltu':x.b<y.b,'leu':x.b<=y.b,
                              'gts':xs>ys,'ges':xs>=ys,'lts':xs<ys,'les':xs<=ys}.get(n)
                    pc=nxt;continue
                if op=='l.mtspr':
                    ra=s.get(regs,reg(a[0]));rb=s.get(regs,reg(a[1]));k=parse_imm(a[2])&M32
                    if ra is not None and ra.const:
                        s.events.append((depth,fn,pc,(ra.b|k)&M32,rb,cond))
                    else:s.unresolved.add((fn,pc,'unknown SPR write address'))
                elif op=='l.mfspr':
                    ra=s.get(regs,reg(a[1]));k=parse_imm(a[2])&M32
                    regs[reg(a[0])]=Val((ra.b|k)&M32,M32,0) if ra is not None and ra.const else None
                elif op=='l.movhi': regs[reg(a[0])]=C(parse_imm(a[1])<<16)
                elif op=='l.lwz':
                    match=re.fullmatch(r'(-?(?:0x[0-9a-f]+|\d+))\(r(\d+)\)',a[1])
                    base=s.get(regs,int(match[2])) if match else None
                    address=(base.b+parse_imm(match[1]))&M32 if base is not None and base.const else None
                    regs[reg(a[0])]=C(s.literal_words[address]) if address in s.literal_words else None
                elif op in('l.addi','l.andi','l.ori','l.xori','l.muli','l.slli','l.srli','l.srai'):
                    imm=parse_imm(a[2]);imm=imm&0xffff if op in('l.andi','l.ori') else imm
                    regs[reg(a[0])]=s.bin(op[2:],s.get(regs,reg(a[1])),C(imm))
                elif op in('l.add','l.sub','l.and','l.or','l.xor','l.mul','l.div','l.divu','l.sll','l.srl','l.sra'):
                    regs[reg(a[0])]=s.bin(op[2:],s.get(regs,reg(a[1])),s.get(regs,reg(a[2])))
                elif op not in NO_DEST and a and a[0].startswith('r'):
                    regs[reg(a[0])]=None
                if 0 in regs: del regs[0]
                pc=nxt
        s.active.remove(fn)

APP_SHA256='9f9b9e62f9ae552106fbc56d848a17de2ab619f6b1e0fb299ce78fb45f0bc3cd'
APP_BASE=0x02000000

def task_records(code,image):
    """Decode the registration at 02000000 and scheduler loads at 02008828.

    This supplies candidate callback roots, not an assertion that every task runs
    during boot. The mutable argument at descriptor+4 is deliberately unknown.
    """
    if hashlib.sha256(image).hexdigest()!=APP_SHA256:
        raise ValueError('expected the verified Silvergear application image')
    registration=Walker(code,1,[0x02008710])
    registration.walk(APP_BASE,{},0)
    result=[]
    for _,_,pc,_,args,_ in registration.calls:
        if not all(args[n] is not None and args[n].const for n in (3,4)):
            raise ValueError('unresolved task registration at '+hex(pc))
        task,address=args[3].b,args[4].b;offset=address-APP_BASE
        if not 0<=offset<=len(image)-20:raise ValueError('task descriptor outside image')
        name,_,init,exit_,poll=struct.unpack_from('<5I',image,offset)
        if not APP_BASE<=name<APP_BASE+len(image):raise ValueError('task name outside image')
        label=image[name-APP_BASE:name-APP_BASE+80].split(b'\0')[0].decode('ascii')
        callbacks=dict(init=init,exit=exit_,poll=poll)
        for target in callbacks.values():
            if target and (target&3 or target not in code):raise ValueError('invalid task callback')
        result.append(dict(id=task,name=label,registration=pc,descriptor=address,callbacks=callbacks))
    if len(result)!=10:raise ValueError('expected ten registered tasks')
    return result

def value_json(v):
    return None if v is None else dict(base=v.base,mask=v.m,bits=v.b)

def verify_listing(path,image):
    """Bind native instruction bytes to the hash-checked application."""
    pattern=re.compile(r'^\s*([0-9a-f]+):\t((?:[0-9a-f]{2} ){4})\t')
    checked=0
    for line in Path(path).read_text().splitlines():
        match=pattern.match(line)
        if not match:continue
        offset=int(match[1],16)-APP_BASE
        if 0<=offset<=len(image)-4:
            if bytes.fromhex(match[2])!=image[offset:offset+4]:
                raise ValueError('listing bytes differ from application at '+match[1])
            checked+=1
    if checked<100:raise ValueError('insufficient native instructions in application listing')
    return checked

def hardware_records(code,image):
    """Hash-bound board device registry and display-mode dispatch candidates.

    02002b18 walks 24-byte records (12-byte name, init, control, mutable
    enabled flag), calling +12 at 02002be0. 02002ce4 calls +16. 02004b90
    selects one of six display-mode callbacks from 02082f08.
    """
    if hashlib.sha256(image).hexdigest()!=APP_SHA256:
        raise ValueError('expected the verified Silvergear application image')
    def words(address,n):
        offset=address-APP_BASE
        if not 0<=offset<=len(image)-4*n:raise ValueError('hardware table outside image')
        return struct.unpack_from('<'+str(n)+'I',image,offset)
    records=[];pointer=words(0x0209ecbc,1)[0]
    for index in range(32):
        address=pointer+24*index;offset=address-APP_BASE
        if not 0<=offset<=len(image)-24:raise ValueError('device descriptor outside image')
        name=image[offset:offset+12].split(b'\0')[0]
        if not name:break
        init,control,_=words(address+12,3)
        records.append(dict(name=name.decode('ascii'),descriptor=address,callbacks=dict(init=init,control=control)))
    else:raise ValueError('unterminated device registry')
    if len(records)!=7:raise ValueError('expected seven device records')
    modes=[dict(name='display mode '+str(index),descriptor=0x02082f08+4*index,callbacks=dict(select=target))
           for index,target in enumerate(words(0x02082f08,6))]
    for record in records+modes:
        for target in record['callbacks'].values():
            if target and (target&3 or target not in code):raise ValueError('invalid hardware callback')
    return records+modes

def describe(addr):
    if 0x8000<=addr<0x8220:
        fam={0:'GPIO.data',0x40:'GPIO.dir(1=in)',0x80:'GPIO.pullup',0xc0:'GPIO.pulldown',0x100:'GPIO.8100',0x140:'GPIO.8140',0x180:'GPIO.8180',0x1c0:'GPIO.force-high',0x200:'GPIO.force-low'}
        f=(addr-0x8000)&~0x3f;b=((addr-0x8000)&0x3f)//4
        return f"{fam.get(f,'GPIO.%x'%(0x8000+f))}[{'ABCDE'[b] if b<5 else b}]"
    if 0x8340<=addr<0x8398: return 'PINMUX'
    if 0x8840<=addr<0x8880: return 'CLK/ANA'
    if 0x9170<=addr<=0x917c: return 'ADC'
    if 0x9830<=addr<=0x9838: return 'TIMER3'
    if 0xd800<=addr<0xd850: return 'USBPHY?'
    return ''

def fmt(v):
    if v is None: return 'unknown'
    if v.const: return f'= {v.b:#010x}'
    return f'= (SPR[{v.base:#06x}] & {v.m:#010x}) | {v.b:#010x}'

def forced(v):
    """Bits the write forces: (set, clear)."""
    return (v.b,(~v.m)&(~v.b)&M32)

def main():
    p=argparse.ArgumentParser(description=__doc__,formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('listing');p.add_argument('--root',action='append',default=[],help='function address; optional :a3,a4.. constants')
    p.add_argument('--tasks',type=Path,help='verified application: follow task, board-device and display-mode callback tables')
    p.add_argument('--json',action='store_true',help='machine-readable evidence, including unresolved calls')
    p.add_argument('--max-depth',type=int,default=10);p.add_argument('--trace-call',action='append',default=[],help='log calls to this address')
    p.add_argument('--range',help='only SPRs lo-hi (hex), e.g. 8840-8880');p.add_argument('--diff',help='dump_sprs.py snapshot JSON')
    a=p.parse_args()
    if not a.root and not a.tasks:p.error('provide --root or --tasks')
    code=load(a.listing)
    if not code: sys.exit('no instructions parsed')
    image=a.tasks.read_bytes() if a.tasks else None
    if image is not None and hashlib.sha256(image).hexdigest()!=APP_SHA256:raise ValueError('application hash mismatch')
    # Immutable six-entry computed-jump table used by 02027aec, not mutable
    # globals or a general assumption that RAM loads retain flash values.
    literal_words={0x020840c4+4*i:struct.unpack_from('<I',image,0x840c4+4*i)[0] for i in range(6)} if image is not None else {}
    w=Walker(code,a.max_depth,[int(x,16) for x in a.trace_call],literal_words);roots=[]
    for root in a.root:
        addr,_,rest=root.partition(':')
        args={3+i:C(int(x,16)) for i,x in enumerate(rest.split(',')) if x} if rest else {}
        roots.append(dict(address=int(addr,16),source='explicit',arguments={n:value_json(v) for n,v in args.items()}))
        w.walk(int(addr,16),args,0)
    tasks=task_records(code,a.tasks.read_bytes()) if a.tasks else []
    listing_checked=verify_listing(a.listing,a.tasks.read_bytes()) if a.tasks else None
    for task in tasks:
        for kind,target in task['callbacks'].items():
            if target:
                roots.append(dict(address=target,source='task '+str(task['id'])+' '+kind))
                w.walk(target,{},0,1)  # selected task/runtime state is not established statically
    hardware=hardware_records(code,a.tasks.read_bytes()) if a.tasks else []
    for record in hardware:
        for kind,target in record['callbacks'].items():
            if target:
                roots.append(dict(address=target,source=record['name']+' '+kind))
                w.walk(target,{},0,1)
    lo,hi=(int(x,16) for x in a.range.split('-')) if a.range else (0,M32)
    snap={int(k,16):int(v,16) for k,v in json.loads(Path(a.diff).read_text()).items()} if a.diff else None
    if a.json:
        writes={};calls={}
        for d,f,pc,addr,v,cond in w.events:
            if not lo<=addr<hi:continue
            key=(f,pc,addr,None if v is None else (v.base,v.m,v.b),bool(cond))
            if key not in writes:writes[key]=dict(function=f,pc=pc,spr=addr,value=value_json(v),conditional=bool(cond),occurrences=0)
            writes[key]['occurrences']+=1
        for _,f,pc,t,args,cond in w.calls:
            entry=dict(function=f,pc=pc,target=t,arguments={n:value_json(v) for n,v in args.items()},conditional=bool(cond))
            calls[json.dumps(entry,sort_keys=True)]=entry
        print(json.dumps(dict(application_sha256=APP_SHA256 if tasks else None,
            listing_sha256=hashlib.sha256(Path(a.listing).read_bytes()).hexdigest(),listing_words_checked=listing_checked,
            roots=roots,tasks=tasks,hardware=hardware,
            limitations='Static candidates, not an execution trace: one visit per PC per call; only the verified analog dispatch table is treated as immutable memory; other loads and return values unknown; unknown branches explored both ways. Callback argument/state and execution order remain unknown.',
            writes=list(writes.values()),calls=list(calls.values()),
            unresolved=[dict(function=f,pc=pc,reason=reason) for f,pc,reason in sorted(w.unresolved)]),indent=2))
        return
    for task in tasks:print('TASK '+str(task['id'])+' '+task['name']+' '+hex(task['descriptor'])+' '+str({k:hex(v) for k,v in task['callbacks'].items()}))
    for record in hardware:print('HARDWARE '+record['name']+' '+hex(record['descriptor'])+' '+str({k:hex(v) for k,v in record['callbacks'].items()}))
    last={}
    for depth,fn,pc,addr,val,cond in w.events:
        if not lo<=addr<hi: continue
        print(f'{"  "*depth}{pc:#x} (fn {fn:#x}) SPR[{addr:#06x}] {fmt(val)}  {describe(addr)}{"  cond="+str(cond) if cond else ""}')
        if val is not None: last[addr]=(pc,val,cond)
    for d,fn,pc,target,args,cond in w.calls:
        print(f'{"  "*d}{pc:#x} call {target:#x}('+', '.join('?' if v is None else (hex(v.b) if v.const else 'spr') for _,v in sorted(args.items()))+')')
    for fn,pc,reason in sorted(w.unresolved):print(f'UNRESOLVED fn={fn:#x} pc={pc:#x}: {reason}',file=sys.stderr)
    if snap is not None:
        print('\n== ledger vs snapshot (last write per SPR) ==')
        for addr,(pc,val,cond) in sorted(last.items()):
            if addr not in snap: continue
            s,c=forced(val);cur=snap[addr]
            bad_set=s&~cur&M32;bad_clr=c&cur
            if bad_set or bad_clr:
                print(f'SPR[{addr:#06x}] {describe(addr):18} snapshot={cur:#010x} stock-write@{pc:#x}{" (cond)" if cond else ""} wants set {s:#010x} clear {c:#010x} -> missing set {bad_set:#010x} stuck set {bad_clr:#010x}')

if __name__=='__main__': main()
