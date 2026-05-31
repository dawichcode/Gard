import sys
data = open('/tmp/field_test.ga', 'rb').read()
p = 4+2+2+2
cpCount = (data[p]<<24)|(data[p+1]<<16)|(data[p+2]<<8)|data[p+3]; p+=4
fnCount = (data[p]<<8)|data[p+1]; p+=2
entry = (data[p]<<8)|data[p+1]; p+=2
codeSize = (data[p]<<24)|(data[p+1]<<16)|(data[p+2]<<8)|data[p+3]; p+=4
for i in range(cpCount):
    tag = data[p]; p+=1
    if tag in (5,6,7): slen=(data[p]<<8)|data[p+1]; p+=2; s=data[p:p+slen].decode(); p+=slen; print(f'CP[{i}] "{s}"')
    elif tag==1: v=(data[p]<<24)|(data[p+1]<<16)|(data[p+2]<<8)|data[p+3]; p+=4; print(f'CP[{i}] int={v}')
    elif tag in (2,4): p+=8
    elif tag==3: p+=4
fns=[]
for i in range(fnCount):
    nl=(data[p]<<8)|data[p+1]; p+=2; nm=data[p:p+nl].decode(); p+=nl
    params=(data[p]<<8)|data[p+1]; p+=2; locs=(data[p]<<8)|data[p+1]; p+=2
    stk=(data[p]<<8)|data[p+1]; p+=2; off=(data[p]<<24)|(data[p+1]<<16)|(data[p+2]<<8)|data[p+3]; p+=4
    ln=(data[p]<<24)|(data[p+1]<<16)|(data[p+2]<<8)|data[p+3]; p+=4; p+=1
    fns.append((nm,off,ln,params,locs))
    print(f'Fn[{i}] "{nm}" p={params} l={locs} off={off} len={ln}')
print(f'Entry={entry}')
code = data[p:p+codeSize]
sizes={0xF0:4,0x20:2,0x21:2,0x13:4,0x82:2,0x81:2,0x80:2,0x70:3,0x71:3,0xE6:6,0xE0:4,0xE1:4,0xE2:4,0xE3:4,0xE4:2,0xE5:2,0xE7:4,0x60:2,0x61:2,0x62:2,0x17:2,0xA0:1,0xA1:2,0xB8:2,0xD0:3,0xD1:1,0x83:2}
names={0xF0:'LINE',0x20:'LOAD',0x21:'STORE',0x13:'I32',0x82:'SETF',0x81:'GETF',0x80:'NEWOBJ',0x70:'CALL',0x71:'VCALL',0xE6:'SCADD',0xE0:'LADD',0x64:'RETV',0x63:'RET',0x02:'DUP',0x01:'POP',0xC0:'PRINT',0x00:'NOP',0x30:'ADD',0x31:'SUB',0xFF:'HALT',0x60:'JMP',0x61:'JIF',0x62:'JIFN',0x10:'NULL'}
for i,(nm,off,ln,params,locs) in enumerate(fns):
    print(f'\n=== {nm} off={off} len={ln} p={params} l={locs} ===')
    pc=off
    while pc < off+ln:
        op=code[pc]; s=pc-off; pc+=1
        sz=sizes.get(op,0)
        operands=code[pc:pc+sz]; pc+=sz
        n=names.get(op,f'x{op:02X}')
        ostr=' '.join(f'{b:02X}' for b in operands)
        print(f'  {s:03X}: {n} {ostr}')
