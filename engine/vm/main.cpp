// GARDVM — Entry point (Tier 15 skeleton, enough to test Tier 4)
#include "interpreter.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

using namespace gardvm;
using namespace gard::bytecode;

// Big-endian deserialization (matches existing gard compiler)
static uint16_t rU16(const uint8_t* p) { return (p[0]<<8)|p[1]; }
static uint32_t rU32(const uint8_t* p) { return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }
static int32_t  rI32(const uint8_t* p) { return (int32_t)rU32(p); }

static BytecodeModule loadGA(const char* path) {
    BytecodeModule mod;
    std::ifstream f(path, std::ios::binary|std::ios::ate);
    if (!f.is_open()) { fprintf(stderr, "Cannot open: %s\n", path); return mod; }
    size_t sz = f.tellg(); f.seekg(0);
    std::vector<uint8_t> data(sz); f.read((char*)data.data(), sz);
    if (sz < 22 || data[0]!='G'||data[1]!='A'||data[2]!='R'||data[3]!='D') return mod;

    const uint8_t* p = data.data() + 4;
    mod.versionMajor = rU16(p); p+=2;
    mod.versionMinor = rU16(p); p+=2;
    mod.flags = rU16(p); p+=2;
    uint32_t cpCount = rU32(p); p+=4;
    uint16_t fnCount = rU16(p); p+=2;
    mod.entryFunction = (int16_t)rU16(p); p+=2;
    uint32_t codeSize = rU32(p); p+=4;

    for (uint32_t i = 0; i < cpCount && p < data.data()+sz; i++) {
        ConstantEntry e; e.tag = (ConstantTag)*p++;
        switch (e.tag) {
            case ConstantTag::Integer: e.intVal = rI32(p); p+=4; break;
            case ConstantTag::Long: { uint64_t v=0; for(int j=0;j<8;j++)v=(v<<8)|*p++; e.longVal=(int64_t)v; break; }
            case ConstantTag::Float: { uint32_t b=rU32(p);p+=4;memcpy(&e.floatVal,&b,4); break; }
            case ConstantTag::Double: { uint64_t b=0;for(int j=0;j<8;j++)b=(b<<8)|*p++;memcpy(&e.doubleVal,&b,8); break; }
            case ConstantTag::String: case ConstantTag::Class: case ConstantTag::Method: {
                uint16_t len=rU16(p);p+=2; e.strVal=std::string((const char*)p,len);p+=len; break; }
        }
        mod.constantPool.push_back(e);
    }
    for (uint16_t i = 0; i < fnCount && p < data.data()+sz; i++) {
        FunctionInfo fn;
        uint16_t nl=rU16(p);p+=2; fn.name=std::string((const char*)p,nl);p+=nl;
        fn.paramCount=rU16(p);p+=2; fn.localCount=rU16(p);p+=2; fn.maxStack=rU16(p);p+=2;
        fn.codeOffset=rU32(p);p+=4; fn.codeLength=rU32(p);p+=4; fn.isAsync=(*p++!=0);
        mod.functions.push_back(fn);
    }
    if (p+codeSize<=data.data()+sz) { mod.code.assign(p,p+codeSize); p+=codeSize; }
    if (p+4<=data.data()+sz) {
        uint32_t dc=rU32(p);p+=4;
        for(uint32_t i=0;i<dc&&p+8<=data.data()+sz;i++){
            DebugSymbol ds; ds.pcOffset=rU32(p);p+=4; ds.line=rU16(p);p+=2; ds.column=rU16(p);p+=2;
            mod.debugSymbols.push_back(ds);
        }
    }
    return mod;
}

int main(int argc, char** argv) {
    if (argc < 3 || strcmp(argv[1],"run")!=0) {
        fprintf(stderr, "Usage: gardvm run <file.ga>\n"); return 1;
    }
    BytecodeModule mod = loadGA(argv[2]);
    if (mod.functions.empty()) { fprintf(stderr, "Failed to load module\n"); return 1; }

    Interpreter vm;
    vm.load(mod);
    vm.registerStdlib();
    int code = vm.run();
    fflush(stdout);
    _Exit(code);
}
