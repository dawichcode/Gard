// GARDVM Tier 4 — Computed-Goto Dispatch Loop (rewritten)
// All hot state in local variables. SYNC/RELOAD around member function calls.
// Big-endian operands matching existing gard bytecode compiler.
#include "interpreter.h"
#include <cstdio>
#include <cstring>

namespace gardvm {
using namespace gard::bytecode;

void Interpreter::execute() {
    // Dispatch table (initialized once)
    static void* T[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; i++) T[i] = &&L_HALT;
        T[0x00]=&&L_NOP; T[0x01]=&&L_POP; T[0x02]=&&L_DUP; T[0x03]=&&L_SWAP;
        T[0x10]=&&L_NULL; T[0x11]=&&L_TRUE; T[0x12]=&&L_FALSE;
        T[0x13]=&&L_I32; T[0x14]=&&L_I64; T[0x15]=&&L_F32; T[0x16]=&&L_F64; T[0x17]=&&L_STR;
        T[0x20]=&&L_LOAD; T[0x21]=&&L_STORE; T[0x22]=&&L_GLOAD; T[0x23]=&&L_GSTORE;
        T[0x30]=&&L_ADD; T[0x31]=&&L_SUB; T[0x32]=&&L_MUL; T[0x33]=&&L_DIV; T[0x34]=&&L_MOD; T[0x35]=&&L_NEG;
        T[0x36]=&&L_FADD; T[0x37]=&&L_FSUB; T[0x38]=&&L_FMUL; T[0x39]=&&L_FDIV; T[0x3A]=&&L_FNEG;
        T[0x40]=&&L_BAND; T[0x41]=&&L_BOR; T[0x42]=&&L_BXOR; T[0x43]=&&L_BNOT;
        T[0x44]=&&L_SHL; T[0x45]=&&L_SHR; T[0x46]=&&L_USHR;
        T[0x50]=&&L_EQ; T[0x51]=&&L_NE; T[0x52]=&&L_LT; T[0x53]=&&L_GT; T[0x54]=&&L_LE; T[0x55]=&&L_GE;
        T[0x58]=&&L_AND; T[0x59]=&&L_OR; T[0x5A]=&&L_NOT;
        T[0x60]=&&L_JMP; T[0x61]=&&L_JIF; T[0x62]=&&L_JIFN; T[0x63]=&&L_RET; T[0x64]=&&L_RETV;
        T[0x70]=&&L_CALL; T[0x71]=&&L_VCALL; T[0x72]=&&L_ACALL;
        T[0x80]=&&L_NEWOBJ; T[0x81]=&&L_GETF; T[0x82]=&&L_SETF;
        T[0x83]=&&L_NEWARR; T[0x84]=&&L_GETE; T[0x85]=&&L_SETE; T[0x86]=&&L_NEWMAP; T[0x87]=&&L_ALEN;
        T[0x90]=&&L_CAT;
        T[0xA0]=&&L_CAST; T[0xA1]=&&L_ISTY; T[0xA2]=&&L_ISNULL;
        T[0xB0]=&&L_AWAIT; T[0xB1]=&&L_YIELD;
        T[0xB8]=&&L_TRY; T[0xB9]=&&L_TRYE; T[0xBA]=&&L_THROW;
        T[0xC0]=&&L_PRINT;
        T[0xD0]=&&L_CLOS; T[0xD1]=&&L_LCAP;
        T[0xE0]=&&L_SADD; T[0xE1]=&&L_SSUB; T[0xE2]=&&L_SCLT; T[0xE3]=&&L_SCGE;
        T[0xE4]=&&L_INC; T[0xE5]=&&L_DEC; T[0xE6]=&&L_SCADD; T[0xE7]=&&L_SSTO;
        T[0xF0]=&&L_LINE; T[0xFF]=&&L_HALT;
        init = true;
    }

    // Hot state in locals (NOT member variables)
    const uint8_t* pc = pc_;
    GValue* sp = sp_;
    GValue* fp = fp_;

    // Bytecode reading macros — comma operator ensures pc advances
    #define U16() (pc+=2, (uint16_t)((pc[-2]<<8)|pc[-1]))
    #define I32() (pc+=4, (int32_t)(((int32_t)pc[-4]<<24)|((int32_t)pc[-3]<<16)|((int32_t)pc[-2]<<8)|(int32_t)pc[-1]))
    #define U8()  (*pc++)

    // Sync locals back to members before calling member functions
    #define SYNC()   do { pc_=pc; sp_=sp; fp_=fp; } while(0)
    #define RELOAD() do { pc=pc_; sp=sp_; fp=fp_; } while(0)

    #define NEXT goto *T[*pc++]

    NEXT;

// ===== Stack =====
L_NOP: NEXT;
L_POP: if(sp>stack_)sp--; NEXT;
L_DUP: sp[0]=sp[-1]; sp++; NEXT;
L_SWAP: { GValue t=sp[-1]; sp[-1]=sp[-2]; sp[-2]=t; NEXT; }

// ===== Constants =====
L_NULL:  *sp++=GVAL_NULL; NEXT;
L_TRUE:  *sp++=GVAL_TRUE; NEXT;
L_FALSE: *sp++=GVAL_FALSE; NEXT;
L_I32: { int32_t v=I32(); *sp++=GValue::makeInt(v); NEXT; }
L_I64: { int32_t v=I32(); *sp++=GValue::makeDouble((double)v); NEXT; }
L_F32: { int32_t b=I32(); float f; memcpy(&f,&b,4); *sp++=GValue::makeDouble((double)f); NEXT; }
L_F64: { int32_t hi=I32(); int32_t lo=I32(); uint64_t b=((uint64_t)(uint32_t)hi<<32)|(uint32_t)lo; double d; memcpy(&d,&b,8); *sp++=GValue::makeDouble(d); NEXT; }
L_STR: { uint16_t i=U16(); SYNC(); GString* s=allocString(module_->constantPool[i].strVal.c_str(),(uint32_t)module_->constantPool[i].strVal.size()); RELOAD(); *sp++=GValue::makePtr(s); NEXT; }

// ===== Locals =====
L_LOAD:  { uint16_t i=U16(); *sp++=fp[i]; NEXT; }
L_STORE: { uint16_t i=U16(); if(sp>stack_){fp[i]=*--sp;}else{fp[i]=GVAL_NULL;} NEXT; }
L_GLOAD: { uint16_t i=U16(); *sp++=locals_[i]; NEXT; }
L_GSTORE:{ uint16_t i=U16(); locals_[i]=*--sp; NEXT; }

// ===== Int Arithmetic =====
L_ADD: { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()+b.asInt()); NEXT; }
L_SUB: { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()-b.asInt()); NEXT; }
L_MUL: { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()*b.asInt()); NEXT; }
L_DIV: { GValue b=*--sp; if(b.asInt()==0){SYNC();throwError("DivisionByZeroError","Division by zero");RELOAD();if(!running_)return;NEXT;} sp[-1]=GValue::makeInt(sp[-1].asInt()/b.asInt()); NEXT; }
L_MOD: { GValue b=*--sp; if(b.asInt()==0){SYNC();throwError("DivisionByZeroError","Modulo by zero");RELOAD();if(!running_)return;NEXT;} sp[-1]=GValue::makeInt(sp[-1].asInt()%b.asInt()); NEXT; }
L_NEG: { sp[-1]=GValue::makeInt(-sp[-1].asInt()); NEXT; }

// ===== Float Arithmetic =====
L_FADD: { GValue b=*--sp; sp[-1]=GValue::makeDouble(sp[-1].toNumber()+b.toNumber()); NEXT; }
L_FSUB: { GValue b=*--sp; sp[-1]=GValue::makeDouble(sp[-1].toNumber()-b.toNumber()); NEXT; }
L_FMUL: { GValue b=*--sp; sp[-1]=GValue::makeDouble(sp[-1].toNumber()*b.toNumber()); NEXT; }
L_FDIV: { GValue b=*--sp; sp[-1]=GValue::makeDouble(sp[-1].toNumber()/b.toNumber()); NEXT; }
L_FNEG: { sp[-1]=GValue::makeDouble(-sp[-1].toNumber()); NEXT; }

// ===== Bitwise =====
L_BAND: { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()&b.asInt()); NEXT; }
L_BOR:  { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()|b.asInt()); NEXT; }
L_BXOR: { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()^b.asInt()); NEXT; }
L_BNOT: { sp[-1]=GValue::makeInt(~sp[-1].asInt()); NEXT; }
L_SHL:  { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()<<b.asInt()); NEXT; }
L_SHR:  { GValue b=*--sp; sp[-1]=GValue::makeInt(sp[-1].asInt()>>b.asInt()); NEXT; }
L_USHR: { GValue b=*--sp; sp[-1]=GValue::makeInt((int32_t)((uint32_t)sp[-1].asInt()>>b.asInt())); NEXT; }

// ===== Comparison =====
L_EQ: { GValue b=*--sp; sp[-1]=GValue::makeBool(sp[-1]==b); NEXT; }
L_NE: { GValue b=*--sp; sp[-1]=GValue::makeBool(sp[-1]!=b); NEXT; }
L_LT: { GValue b=*--sp; GValue a=sp[-1]; sp[-1]=GValue::makeBool(a.isInt()&&b.isInt()?a.asInt()<b.asInt():a.toNumber()<b.toNumber()); NEXT; }
L_GT: { GValue b=*--sp; GValue a=sp[-1]; sp[-1]=GValue::makeBool(a.isInt()&&b.isInt()?a.asInt()>b.asInt():a.toNumber()>b.toNumber()); NEXT; }
L_LE: { GValue b=*--sp; GValue a=sp[-1]; sp[-1]=GValue::makeBool(a.isInt()&&b.isInt()?a.asInt()<=b.asInt():a.toNumber()<=b.toNumber()); NEXT; }
L_GE: { GValue b=*--sp; GValue a=sp[-1]; sp[-1]=GValue::makeBool(a.isInt()&&b.isInt()?a.asInt()>=b.asInt():a.toNumber()>=b.toNumber()); NEXT; }

// ===== Logical =====
L_AND: { GValue b=*--sp; sp[-1]=GValue::makeBool(!sp[-1].isFalsy()&&!b.isFalsy()); NEXT; }
L_OR:  { GValue b=*--sp; sp[-1]=GValue::makeBool(!sp[-1].isFalsy()||!b.isFalsy()); NEXT; }
L_NOT: { sp[-1]=GValue::makeBool(sp[-1].isFalsy()); NEXT; }

// ===== Control Flow =====
L_JMP:  { uint16_t off=U16(); pc=module_->code.data()+off; NEXT; }
L_JIF:  { uint16_t off=U16(); GValue c=*--sp; if(!c.isFalsy()){pc=module_->code.data()+off;} NEXT; }
L_JIFN: { uint16_t off=U16(); GValue c=*--sp; if(c.isFalsy()){pc=module_->code.data()+off;} NEXT; }
L_RET:  { GValue rv=*--sp; SYNC(); popFrame(); RELOAD(); if(!running_)return; *sp++=rv; NEXT; }
L_RETV: { SYNC(); popFrame(); RELOAD(); if(!running_)return; *sp++=GVAL_NULL; NEXT; }

// ===== Calls =====
L_CALL: { uint16_t ni=U16(); uint8_t ac=U8(); const std::string& name=module_->constantPool[ni].strVal;
    { auto fi=funcLookup_.find(name); if(fi!=funcLookup_.end()){SYNC();pushFrame(fi->second,ac);RELOAD();if(!running_)return;NEXT;} }
    { auto nit=natives_.find(name); if(nit!=natives_.end()){GValue* args=sp-ac;GValue r=nit->second(args,ac);sp=args;*sp++=r;NEXT;} }
    SYNC();throwError("UndefinedFunctionError",name.c_str());RELOAD();if(!running_)return; NEXT; }

L_VCALL: { uint16_t ni=U16(); uint8_t ac=U8(); const std::string& name=module_->constantPool[ni].strVal;
    { auto nit=natives_.find(name); if(nit!=natives_.end()){GValue* args=sp-ac;GValue r=nit->second(args,ac);sp=args;*sp++=r;NEXT;} }
    { auto fi=funcLookup_.find(name); if(fi!=funcLookup_.end()){SYNC();pushFrame(fi->second,ac);RELOAD();if(!running_)return;NEXT;} }
    // Method dispatch
    if(ac>0){GValue recv=*(sp-ac); if(recv.isPtr()){void* ptr=recv.asPtr(); if(ptr){ObjHeader* hdr=(ObjHeader*)ptr; if(hdr->type()==ObjType::Object){
        uint64_t key=((uint64_t)hdr->shapeId()<<32)|(uint64_t)ni;
        auto mfi=methodTable_.find(key); if(mfi!=methodTable_.end()){SYNC();pushFrame(mfi->second,ac);RELOAD();if(!running_)return;NEXT;}
    }}}}
    // Error constructors
    if(name.size()>5&&name.compare(name.size()-5,5,"Error")==0&&name.compare(0,4,"Gard")==0){
        SYNC(); GObject* err=allocObject(0); RELOAD();
        if(err){for(uint16_t ci=0;ci<(uint16_t)module_->constantPool.size();ci++){if(module_->constantPool[ci].tag==ConstantTag::String){const std::string& s=module_->constantPool[ci].strVal;if(s=="type"&&ci<32){SYNC();err->fields[ci]=GValue::makePtr(allocString(name.c_str(),(uint32_t)name.size()));RELOAD();}else if(s=="message"&&ci<32&&ac>0){err->fields[ci]=*(sp-ac);}else if(s=="line"&&ci<32){err->fields[ci]=GValue::makeInt((int32_t)currentLine_);}else if(s=="column"&&ci<32){err->fields[ci]=GValue::makeInt((int32_t)currentCol_);}}}sp-=ac;*sp++=GValue::makePtr(err);NEXT;}
    }
    SYNC();throwError("UndefinedFunctionError",name.c_str());RELOAD();if(!running_)return; NEXT; }

L_ACALL: { uint16_t ni=U16(); uint8_t ac=U8(); const std::string& name=module_->constantPool[ni].strVal;
    { auto fi=funcLookup_.find(name); if(fi!=funcLookup_.end()){SYNC();pushFrame(fi->second,ac);RELOAD();if(!running_)return;NEXT;} }
    { auto nit=natives_.find(name); if(nit!=natives_.end()){GValue* args=sp-ac;GValue r=nit->second(args,ac);sp=args;*sp++=r;NEXT;} }
    SYNC();throwError("UndefinedFunctionError",name.c_str());RELOAD();if(!running_)return; NEXT; }

// ===== Objects =====
L_NEWOBJ: { uint16_t ci=U16(); const std::string& cn=module_->constantPool[ci].strVal; uint32_t sid=shapes_.getIdByName(cn); if(sid==0)sid=shapes_.createShape(cn,16); SYNC();GObject* o=allocObject(sid);RELOAD(); *sp++=GValue::makePtr(o); NEXT; }
L_GETF: { uint16_t ni=U16(); GValue ov=*--sp; if(!ov.isPtr()||!ov.asPtr()){*sp++=GVAL_NULL;NEXT;} GObject* o=(GObject*)ov.asPtr(); if(o->header.type()==ObjType::Object){*sp++=getField(o,ni);}else{*sp++=GVAL_NULL;} NEXT; }
L_SETF: { uint16_t ni=U16(); GValue val=*--sp; GValue ov=*--sp; if(!ov.isPtr()||!ov.asPtr()){NEXT;} GObject* o=(GObject*)ov.asPtr(); if(o->header.type()==ObjType::Object){setField(o,ni,val);} NEXT; }
L_NEWARR: { uint16_t sz=U16(); SYNC();GArray* a=allocArray(sz);RELOAD(); for(int i=sz-1;i>=0;i--)a->data[i]=*--sp; a->length=sz; *sp++=GValue::makePtr(a); NEXT; }
L_GETE: { GValue idx=*--sp; GValue av=sp[-1]; if(!av.isPtr()||!av.asPtr()){sp[-1]=GVAL_NULL;NEXT;} GArray* a=(GArray*)av.asPtr(); int32_t i=idx.asInt(); if(i<0||i>=a->length){SYNC();throwError("IndexOutOfBoundsError","Array index out of bounds");RELOAD();if(!running_)return;NEXT;} sp[-1]=a->data[i]; NEXT; }
L_SETE: { GValue val=*--sp; GValue idx=*--sp; GValue av=*--sp; if(!av.isPtr()||!av.asPtr()){SYNC();throwError("NullReferenceError","Cannot index null");RELOAD();if(!running_)return;NEXT;} GArray* a=(GArray*)av.asPtr(); int32_t i=idx.asInt(); if(i<0||i>=a->length){SYNC();throwError("IndexOutOfBoundsError","Array index out of bounds");RELOAD();if(!running_)return;NEXT;} a->data[i]=val; NEXT; }
L_NEWMAP: { *sp++=GVAL_NULL; NEXT; }
L_ALEN: { GValue av=sp[-1]; if(av.isPtr()&&av.asPtr()){GArray* a=(GArray*)av.asPtr();sp[-1]=GValue::makeInt(a->length);}else{sp[-1]=GVAL_ZERO;} NEXT; }

// ===== String =====
L_CAT: { GValue b=*--sp; GValue a=sp[-1]; if(a.isPtr()&&b.isPtr()){SYNC();sp[-1]=GValue::makePtr(concatStrings((GString*)a.asPtr(),(GString*)b.asPtr()));RELOAD();} NEXT; }

// ===== Type Ops =====
L_CAST: { uint8_t t=U8(); GValue v=sp[-1]; if(t==0)sp[-1]=GValue::makeInt(v.toInt32()); else if(t==1)sp[-1]=GValue::makeDouble(v.toNumber()); else sp[-1]=GValue::makeBool(!v.isFalsy()); NEXT; }
L_ISTY: { uint16_t ti=U16(); GValue v=sp[-1]; if(v.isPtr()&&v.asPtr()){GObject* o=(GObject*)v.asPtr();sp[-1]=GValue::makeBool(o->header.shapeId()==ti);}else if(v.isInt()){sp[-1]=GValue::makeBool(ti==0xFFFF);}else{sp[-1]=GVAL_FALSE;} NEXT; }
L_ISNULL: { sp[-1]=GValue::makeBool(sp[-1].isNull()); NEXT; }

// ===== Async (sync execution) =====
L_AWAIT: NEXT;
L_YIELD: NEXT;

// ===== Exceptions =====
L_TRY: { uint16_t off=U16(); handlers_.push_back({module_->code.data()+off,sp,frameTop_}); NEXT; }
L_TRYE: { if(!handlers_.empty())handlers_.pop_back(); NEXT; }
L_THROW: { currentException_=*--sp; SYNC(); if(!unwindToHandler()){fprintf(stderr,"\033[1;31mUnhandled exception\033[0m\n");running_=false;exitCode_=1;return;} RELOAD(); NEXT; }

// ===== Print =====
L_PRINT: { GValue v=*--sp; if(v.isInt())printf("%d\n",v.asInt()); else if(v.isDouble())printf("%g\n",v.asDouble()); else if(v.isBool())printf("%s\n",v.asBool()?"true":"false"); else if(v.isNull())printf("null\n"); else if(v.isPtr()&&v.asPtr()){ObjHeader* h=(ObjHeader*)v.asPtr();if(h->type()==ObjType::String){GString* s=(GString*)v.asPtr();const char* d=s->data();if(d)printf("%.*s\n",(int)s->length,d);else printf("<rope>\n");}else printf("<object>\n");}else printf("null\n"); NEXT; }

// ===== Closures =====
L_CLOS: { uint16_t fi=U16(); uint8_t cc=U8(); size_t sz=sizeof(GClosure)+cc*sizeof(GValue); SYNC();GClosure* cl=(GClosure*)gc_.allocNursery(sz);RELOAD(); cl->header=ObjHeader::make(ObjType::Closure,0,(uint32_t)sz); cl->funcIndex=fi; cl->captureCount=cc; for(int i=cc-1;i>=0;i--)cl->captures[i]=*--sp; *sp++=GValue::makePtr(cl); NEXT; }
L_LCAP: { uint8_t i=U8(); GClosure* cl=frameTop_->closure; if(cl&&i<cl->captureCount)*sp++=cl->captures[i]; else *sp++=GVAL_NULL; NEXT; }

// ===== Superinstructions (u16 operands) =====
L_SADD: { uint16_t a=U16(); uint16_t b=U16(); *sp++=GValue::makeInt(fp[a].asInt()+fp[b].asInt()); NEXT; }
L_SSUB: { uint16_t a=U16(); uint16_t b=U16(); *sp++=GValue::makeInt(fp[a].asInt()-fp[b].asInt()); NEXT; }
L_SCLT: { uint16_t a=U16(); uint16_t b=U16(); *sp++=GValue::makeBool(fp[a].asInt()<fp[b].asInt()); NEXT; }
L_SCGE: { uint16_t a=U16(); uint16_t b=U16(); *sp++=GValue::makeBool(fp[a].asInt()>=fp[b].asInt()); NEXT; }
L_INC:  { uint16_t i=U16(); fp[i]=GValue::makeInt(fp[i].asInt()+1); NEXT; }
L_DEC:  { uint16_t i=U16(); fp[i]=GValue::makeInt(fp[i].asInt()-1); NEXT; }
L_SCADD:{ uint16_t i=U16(); int32_t n=I32(); *sp++=GValue::makeInt(fp[i].asInt()+n); NEXT; }
L_SSTO: { uint16_t s=U16(); uint16_t d=U16(); fp[d]=fp[s]; NEXT; }

// ===== Debug =====
L_LINE: { currentLine_=(pc[0]<<8)|pc[1]; currentCol_=(pc[2]<<8)|pc[3]; pc+=4; NEXT; }

L_HALT: SYNC(); running_=false; return;

    #undef U16
    #undef I32
    #undef U8
    #undef SYNC
    #undef RELOAD
    #undef NEXT
}

} // namespace gardvm
