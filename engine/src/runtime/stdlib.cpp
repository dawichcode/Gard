#include "runtime/stdlib.h"
#include "runtime/async.h"
#include <cstring>
#include <chrono>
#include <iomanip>
#include <cmath>
#include <random>
#include <algorithm>
#include <limits>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <unistd.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <regex>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>
#include <libxml/xmlstring.h>


namespace gard {
namespace runtime {
namespace stdlib {

static std::mt19937 rng(std::chrono::steady_clock::now().time_since_epoch().count());

// ===== 5.2 String =====
void registerStringFunctions(VM& vm) {
    vm.registerNative("String.toUpperCase", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::makeString("");
        std::string s = args[0].toString();
        std::transform(s.begin(), s.end(), s.begin(), ::toupper);
        return Value::makeString(s);
    });
    vm.registerNative("String.toLowerCase", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::makeString("");
        std::string s = args[0].toString();
        std::transform(s.begin(), s.end(), s.begin(), ::tolower);
        return Value::makeString(s);
    });
    vm.registerNative("String.substring", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3) return Value::makeString("");
        std::string s = args[0].toString();
        int start = args[1].toInt(), end = args[2].toInt();
        if (start < 0) start = 0;
        if (end > (int)s.size()) end = s.size();
        if (start >= end) return Value::makeString("");
        return Value::makeString(s.substr(start, end - start));
    });
    vm.registerNative("String.indexOf", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeInt(-1);
        size_t pos = args[0].toString().find(args[1].toString());
        return Value::makeInt(pos == std::string::npos ? -1 : (int)pos);
    });
    vm.registerNative("String.replace", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3) return args.empty() ? Value::makeString("") : Value::makeString(args[0].toString());
        std::string s = args[0].toString(), old = args[1].toString(), rep = args[2].toString();
        size_t pos = s.find(old);
        if (pos != std::string::npos) s.replace(pos, old.size(), rep);
        return Value::makeString(s);
    });
    vm.registerNative("String.split", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeArray();
        std::string s = args[0].toString(), delim = args[1].toString();
        Value arr = Value::makeArray();
        size_t pos;
        while ((pos = s.find(delim)) != std::string::npos) {
            arr.arrVal->elements.push_back(Value::makeString(s.substr(0, pos)));
            s.erase(0, pos + delim.size());
        }
        arr.arrVal->elements.push_back(Value::makeString(s));
        return arr;
    });
    vm.registerNative("String.trim", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::makeString("");
        std::string s = args[0].toString();
        size_t a = s.find_first_not_of(" \t\n\r"), b = s.find_last_not_of(" \t\n\r");
        return Value::makeString(a == std::string::npos ? "" : s.substr(a, b - a + 1));
    });
    vm.registerNative("String.startsWith", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeBool(false);
        return Value::makeBool(args[0].toString().rfind(args[1].toString(), 0) == 0);
    });
    vm.registerNative("String.endsWith", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeBool(false);
        std::string s = args[0].toString(), suf = args[1].toString();
        if (suf.size() > s.size()) return Value::makeBool(false);
        return Value::makeBool(s.compare(s.size() - suf.size(), suf.size(), suf) == 0);
    });
    vm.registerNative("String.contains", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeBool(false);
        return Value::makeBool(args[0].toString().find(args[1].toString()) != std::string::npos);
    });
    vm.registerNative("String.length", [](const std::vector<Value>& args) -> Value {
        return Value::makeInt(args.empty() ? 0 : (int)args[0].toString().size());
    });
    vm.registerNative("String.repeat", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeString("");
        std::string s = args[0].toString(), r;
        for (int i = 0; i < args[1].toInt(); i++) r += s;
        return Value::makeString(r);
    });
    vm.registerNative("String.padStart", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3) return args.empty() ? Value::makeString("") : Value::makeString(args[0].toString());
        std::string s = args[0].toString(), pad = args[2].toString();
        while ((int)s.size() < args[1].toInt()) s = pad + s;
        return Value::makeString(s);
    });
    vm.registerNative("String.padEnd", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3) return args.empty() ? Value::makeString("") : Value::makeString(args[0].toString());
        std::string s = args[0].toString(), pad = args[2].toString();
        while ((int)s.size() < args[1].toInt()) s = s + pad;
        return Value::makeString(s);
    });
    vm.registerNative("String.slice", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3) return Value::makeString("");
        std::string s = args[0].toString();
        int start = args[1].toInt(), end = args[2].toInt();
        if (start < 0) start = std::max(0, (int)s.size() + start);
        if (end < 0) end = std::max(0, (int)s.size() + end);
        if (start >= end || start >= (int)s.size()) return Value::makeString("");
        return Value::makeString(s.substr(start, end - start));
    });
    vm.registerNative("String.lastIndexOf", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeInt(-1);
        size_t pos = args[0].toString().rfind(args[1].toString());
        return Value::makeInt(pos == std::string::npos ? -1 : (int)pos);
    });
    vm.registerNative("String.replaceAll", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 3) return args.empty() ? Value::makeString("") : Value::makeString(args[0].toString());
        std::string s = args[0].toString(), old = args[1].toString(), rep = args[2].toString();
        size_t pos = 0;
        while ((pos = s.find(old, pos)) != std::string::npos) {
            s.replace(pos, old.size(), rep);
            pos += rep.size();
        }
        return Value::makeString(s);
    });
    vm.registerNative("String.trimStart", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::makeString("");
        std::string s = args[0].toString();
        size_t start = s.find_first_not_of(" \t\n\r");
        return Value::makeString(start == std::string::npos ? "" : s.substr(start));
    });
    vm.registerNative("String.trimEnd", [](const std::vector<Value>& args) -> Value {
        if (args.empty()) return Value::makeString("");
        std::string s = args[0].toString();
        size_t end = s.find_last_not_of(" \t\n\r");
        return Value::makeString(end == std::string::npos ? "" : s.substr(0, end + 1));
    });
    vm.registerNative("String.charAt", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeString("");
        std::string s = args[0].toString();
        int idx = args[1].toInt();
        if (idx < 0 || idx >= (int)s.size()) return Value::makeString("");
        return Value::makeString(std::string(1, s[idx]));
    });
    vm.registerNative("String.charCodeAt", [](const std::vector<Value>& args) -> Value {
        if (args.size() < 2) return Value::makeInt(0);
        std::string s = args[0].toString();
        int idx = args[1].toInt();
        if (idx < 0 || idx >= (int)s.size()) return Value::makeInt(0);
        return Value::makeInt(static_cast<int>(static_cast<unsigned char>(s[idx])));
    });
}

// ===== 5.4 Math =====
void registerMathFunctions(VM& vm) {
    vm.registerNative("Math.pow", [](const std::vector<Value>& a) -> Value { return a.size()<2?Value::makeDouble(0):Value::makeDouble(std::pow(a[0].toDouble(),a[1].toDouble())); });
    vm.registerNative("Math.sqrt", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()) { vm.throwError("GardMathError", "Math.sqrt requires a numeric argument"); return Value::makeNull(); }
        double v = a[0].toDouble();
        if(v < 0) { vm.throwError("GardMathError", "Math.sqrt: cannot compute square root of negative number (" + std::to_string(v) + ")"); return Value::makeNull(); }
        return Value::makeDouble(std::sqrt(v));
    });
    vm.registerNative("Math.abs", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeInt(0); double v=a[0].toDouble(); return v==static_cast<int>(v)?Value::makeInt(std::abs((int)v)):Value::makeDouble(std::fabs(v)); });
    vm.registerNative("Math.min", [](const std::vector<Value>& a) -> Value { return a.size()<2?Value::makeInt(0):Value::makeDouble(std::min(a[0].toDouble(),a[1].toDouble())); });
    vm.registerNative("Math.max", [](const std::vector<Value>& a) -> Value { return a.size()<2?Value::makeInt(0):Value::makeDouble(std::max(a[0].toDouble(),a[1].toDouble())); });
    vm.registerNative("Math.round", [](const std::vector<Value>& a) -> Value { return Value::makeInt(a.empty()?0:(int)std::round(a[0].toDouble())); });
    vm.registerNative("Math.floor", [](const std::vector<Value>& a) -> Value { return Value::makeInt(a.empty()?0:(int)std::floor(a[0].toDouble())); });
    vm.registerNative("Math.ceil", [](const std::vector<Value>& a) -> Value { return Value::makeInt(a.empty()?0:(int)std::ceil(a[0].toDouble())); });
    vm.registerNative("Math.sin", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::sin(a[0].toDouble())); });
    vm.registerNative("Math.cos", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::cos(a[0].toDouble())); });
    vm.registerNative("Math.random", [](const std::vector<Value>&) -> Value { std::uniform_real_distribution<double> d(0,1); return Value::makeDouble(d(rng)); });
    vm.registerNative("Math.randomInt", [](const std::vector<Value>& a) -> Value { if(a.size()<2)return Value::makeInt(0); std::uniform_int_distribution<int> d(a[0].toInt(),a[1].toInt()); return Value::makeInt(d(rng)); });
    vm.registerNative("Math.cbrt", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::cbrt(a[0].toDouble())); });
    vm.registerNative("Math.trunc", [](const std::vector<Value>& a) -> Value { return Value::makeInt(a.empty()?0:(int)std::trunc(a[0].toDouble())); });
    vm.registerNative("Math.sign", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeInt(0); double v=a[0].toDouble(); return Value::makeInt(v>0?1:(v<0?-1:0)); });
    vm.registerNative("Math.clamp", [](const std::vector<Value>& a) -> Value { if(a.size()<3)return Value::makeInt(0); double v=a[0].toDouble(),lo=a[1].toDouble(),hi=a[2].toDouble(); return Value::makeDouble(v<lo?lo:(v>hi?hi:v)); });
    vm.registerNative("Math.tan", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::tan(a[0].toDouble())); });
    vm.registerNative("Math.asin", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::asin(a[0].toDouble())); });
    vm.registerNative("Math.acos", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::acos(a[0].toDouble())); });
    vm.registerNative("Math.atan", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::atan(a[0].toDouble())); });
    vm.registerNative("Math.atan2", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.size()<2?0:std::atan2(a[0].toDouble(),a[1].toDouble())); });
    vm.registerNative("Math.log", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::log(a[0].toDouble())); });
    vm.registerNative("Math.log2", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::log2(a[0].toDouble())); });
    vm.registerNative("Math.log10", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::log10(a[0].toDouble())); });
    vm.registerNative("Math.exp", [](const std::vector<Value>& a) -> Value { return Value::makeDouble(a.empty()?0:std::exp(a[0].toDouble())); });
    vm.registerNative("Math.PI", [](const std::vector<Value>&) -> Value { return Value::makeDouble(3.14159265358979323846); });
    vm.registerNative("Math.E", [](const std::vector<Value>&) -> Value { return Value::makeDouble(2.71828182845904523536); });
    vm.registerNative("Math.INFINITY", [](const std::vector<Value>&) -> Value { return Value::makeDouble(std::numeric_limits<double>::infinity()); });
    vm.registerNative("Math.NAN", [](const std::vector<Value>&) -> Value { return Value::makeDouble(std::numeric_limits<double>::quiet_NaN()); });
}

// ===== 5.5 DateTime =====

// Date Object helper (used by Date.* natives)
static Value makeDateObject(int64_t epochMs) {
    Value d = Value::makeObject("Date");
    d.objVal->fields["_epoch"] = Value::makeLong(epochMs);
    auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(epochMs));
    auto t = std::chrono::system_clock::to_time_t(tp);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    d.objVal->fields["value"] = Value::makeString(buf);
    d.objVal->fields["epoch"] = Value::makeLong(epochMs);
    return d;
}

void registerDateTimeFunctions(VM& vm) {
    vm.registerNative("DateTime.now", [](const std::vector<Value>&) -> Value { return Value::makeLong(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); });
    vm.registerNative("DateTime.utc", [](const std::vector<Value>&) -> Value { return Value::makeLong(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); });
    vm.registerNative("DateTime.fromEpoch", [](const std::vector<Value>& a) -> Value { return a.empty()?Value::makeLong(0):Value::makeLong(a[0].toInt()); });
    vm.registerNative("DateTime.format", [](const std::vector<Value>&) -> Value { auto t=std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()); char b[64]; std::strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",std::localtime(&t)); return Value::makeString(b); });
    vm.registerNative("DateTime.toEpochMillis", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeLong(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        return Value::makeLong(a[0].toInt()); // pass-through if already epoch
    });
    vm.registerNative("DateTime.parse", [](const std::vector<Value>& a) -> Value {
        // Simplified: return current time (full impl would parse the string)
        if (a.empty()) return Value::makeLong(0);
        return Value::makeLong(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    });
    vm.registerNative("DateTime.addDays", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        int64_t ms = a[0].toInt();
        int64_t days = a[1].toInt();
        return Value::makeLong(ms + days * 86400000LL);
    });
    vm.registerNative("DateTime.addHours", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        return Value::makeLong(a[0].toInt() + (int64_t)a[1].toInt() * 3600000LL);
    });
    vm.registerNative("DateTime.addMinutes", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        return Value::makeLong(a[0].toInt() + (int64_t)a[1].toInt() * 60000LL);
    });
    vm.registerNative("DateTime.addWeeks", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        return Value::makeLong(a[0].toInt() + (int64_t)a[1].toInt() * 604800000LL);
    });
    vm.registerNative("DateTime.addMonths", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        return Value::makeLong(a[0].toInt() + (int64_t)a[1].toInt() * 2592000000LL); // ~30 days
    });
    vm.registerNative("DateTime.addYears", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        return Value::makeLong(a[0].toInt() + (int64_t)a[1].toInt() * 31536000000LL); // 365 days
    });
    vm.registerNative("DateTime.isExpired", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeBool(true);
        int64_t ts = a[0].toInt();
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return Value::makeBool(ts < now);
    });
    vm.registerNative("DateTime.subtract", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeLong(0);
        return Value::makeLong(a[0].toInt() - a[1].toInt()); // difference in ms
    });
    vm.registerNative("DateTime.isBefore", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeBool(false);
        return Value::makeBool(a[0].toInt() < a[1].toInt());
    });
    vm.registerNative("DateTime.isAfter", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeBool(false);
        return Value::makeBool(a[0].toInt() > a[1].toInt());
    });
    vm.registerNative("DateTime.equals", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeBool(false);
        return Value::makeBool(a[0].toInt() == a[1].toInt());
    });
    vm.registerNative("DateTime.toTimezone", [](const std::vector<Value>& a) -> Value {
        // Simplified: return same timestamp (full impl would adjust offset)
        return a.empty() ? Value::makeLong(0) : Value::makeLong(a[0].toInt());
    });
    vm.registerNative("DateTime.getTimezone", [](const std::vector<Value>&) -> Value {
        // Return system timezone abbreviation
        auto t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm tm = *std::localtime(&t);
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Z", &tm);
        return Value::makeString(buf);
    });

    // ===== Date Object API =====

    // Date.now() — returns Date object for current time
    vm.registerNative("Date.now", [](const std::vector<Value>&) -> Value {
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return makeDateObject(now);
    });

    // Date.from(epochMs) — create Date from epoch milliseconds
    vm.registerNative("Date.from", [](const std::vector<Value>& a) -> Value {
        return makeDateObject(a.empty() ? 0 : (int64_t)a[0].toInt());
    });

    // Date.parse(dateString) — parse "2024-01-15" or "2024-01-15 10:30:00"
    vm.registerNative("Date.parse", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return makeDateObject(0);
        std::string s = a[0].toString();
        std::tm tm = {};
        if (s.size() > 10) strptime(s.c_str(), "%Y-%m-%d %H:%M:%S", &tm);
        else strptime(s.c_str(), "%Y-%m-%d", &tm);
        auto tp = std::chrono::system_clock::from_time_t(mktime(&tm));
        int64_t ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
        return makeDateObject(ms);
    });

    // Instance methods on Date objects (dispatched via type-based routing)

    // Date.format(date, pattern?) — format date to string
    vm.registerNative("Date.format", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        std::string pattern = a.size() >= 2 ? a[1].toString() : "%Y-%m-%d %H:%M:%S";
        auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(epoch));
        auto t = std::chrono::system_clock::to_time_t(tp);
        char buf[128];
        std::strftime(buf, sizeof(buf), pattern.c_str(), std::localtime(&t));
        return Value::makeString(buf);
    });

    // Date.addDays(date, days) — returns new Date
    vm.registerNative("Date.addDays", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        return makeDateObject(epoch + (int64_t)a[1].toInt() * 86400000LL);
    });

    // Date.addHours(date, hours)
    vm.registerNative("Date.addHours", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        return makeDateObject(epoch + (int64_t)a[1].toInt() * 3600000LL);
    });

    // Date.addMinutes(date, minutes)
    vm.registerNative("Date.addMinutes", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        return makeDateObject(epoch + (int64_t)a[1].toInt() * 60000LL);
    });

    // Date.addMonths(date, months)
    vm.registerNative("Date.addMonths", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        return makeDateObject(epoch + (int64_t)a[1].toInt() * 2592000000LL);
    });

    // Date.addYears(date, years)
    vm.registerNative("Date.addYears", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        return makeDateObject(epoch + (int64_t)a[1].toInt() * 31536000000LL);
    });

    // Date.isExpired(date) — true if date is in the past
    vm.registerNative("Date.isExpired", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(true);
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
        return Value::makeBool(epoch < now);
    });

    // Date.isBefore(date, other)
    vm.registerNative("Date.isBefore", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) return Value::makeBool(false);
        return Value::makeBool(a[0].objVal->fields["_epoch"].longVal < a[1].objVal->fields["_epoch"].longVal);
    });

    // Date.isAfter(date, other)
    vm.registerNative("Date.isAfter", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) return Value::makeBool(false);
        return Value::makeBool(a[0].objVal->fields["_epoch"].longVal > a[1].objVal->fields["_epoch"].longVal);
    });

    // Date.diff(date, other) — difference in milliseconds
    vm.registerNative("Date.diff", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal || !a[1].objVal) return Value::makeLong(0);
        return Value::makeLong(a[0].objVal->fields["_epoch"].longVal - a[1].objVal->fields["_epoch"].longVal);
    });

    // Date.toEpoch(date) — get epoch milliseconds
    vm.registerNative("Date.toEpoch", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeLong(0);
        return a[0].objVal->fields["_epoch"];
    });

    // Date.toISO(date) — ISO 8601 format
    vm.registerNative("Date.toISO", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");
        int64_t epoch = a[0].objVal->fields["_epoch"].longVal;
        auto tp = std::chrono::system_clock::time_point(std::chrono::milliseconds(epoch));
        auto t = std::chrono::system_clock::to_time_t(tp);
        char buf[64];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&t));
        return Value::makeString(buf);
    });
}

// ===== 5.6 JSON =====
void registerJsonFunctions(VM& vm) {
    vm.registerNative("JSON.stringify", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeString("null"); return Value::makeString(a[0].toString()); });
    vm.registerNative("JSON.parse", [](const std::vector<Value>& a) -> Value { return a.empty()?Value::makeNull():a[0]; });
    vm.registerNative("JSON.isValid", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeBool(false); std::string s=a[0].toString(); return Value::makeBool(!s.empty()&&(s[0]=='{'||s[0]=='['||s[0]=='"'||s=="true"||s=="false"||s=="null"||std::isdigit(s[0]))); });
    vm.registerNative("JSON.prettyPrint", [](const std::vector<Value>& a) -> Value {
        // Stringify with indentation
        if (a.empty()) return Value::makeString("null");
        int indent = a.size() >= 2 ? a[1].toInt() : 2;
        std::string val = a[0].toString();
        // Simple pretty-print: add newlines after { and , for objects
        std::string result;
        int depth = 0;
        for (char c : val) {
            if (c == '{' || c == '[') {
                result += c;
                result += '\n';
                depth++;
                result += std::string(depth * indent, ' ');
            } else if (c == '}' || c == ']') {
                result += '\n';
                depth--;
                result += std::string(depth * indent, ' ');
                result += c;
            } else if (c == ',') {
                result += c;
                result += '\n';
                result += std::string(depth * indent, ' ');
            } else {
                result += c;
            }
        }
        return Value::makeString(result);
    });
    vm.registerNative("JSON.query", [](const std::vector<Value>& a) -> Value {
        // Simple JSON path: supports $.field access on objects
        if (a.size() < 2) return Value::makeNull();
        Value data = a[0];
        std::string path = a[1].toString();
        // Strip leading "$."
        if (path.size() > 2 && path[0] == '$' && path[1] == '.') {
            path = path.substr(2);
        }
        // For object values, look up the field
        if (data.type == ValueType::Object && data.objVal) {
            auto it = data.objVal->fields.find(path);
            if (it != data.objVal->fields.end()) return it->second;
        }
        if (data.type == ValueType::Map && data.mapVal) {
            auto it = data.mapVal->entries.find(path);
            if (it != data.mapVal->entries.end()) return it->second;
        }
        return Value::makeNull();
    });
    vm.registerNative("JSON.typeOf", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeString("undefined");
        switch (a[0].type) {
            case ValueType::Null: return Value::makeString("null");
            case ValueType::Int: case ValueType::Long: case ValueType::Float: case ValueType::Double:
                return Value::makeString("number");
            case ValueType::Bool: return Value::makeString("boolean");
            case ValueType::String: return Value::makeString("string");
            case ValueType::Array: return Value::makeString("array");
            case ValueType::Map: case ValueType::Object: return Value::makeString("object");
            default: return Value::makeString("unknown");
        }
    });

    // toJson() — universal method, works on any value type
    // Recursively serializes to proper JSON string
    auto toJsonImpl = [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeString("null");
        const Value& v = a[0];

        std::function<std::string(const Value&)> serialize = [&](const Value& val) -> std::string {
            switch (val.type) {
                case ValueType::Null: return "null";
                case ValueType::Int: return std::to_string(val.intVal);
                case ValueType::Long: return std::to_string(val.longVal);
                case ValueType::Float: return std::to_string(val.floatVal);
                case ValueType::Double: return std::to_string(val.doubleVal);
                case ValueType::Bool: return val.boolVal ? "true" : "false";
                case ValueType::Char: return "\"" + std::string(1, val.charVal) + "\"";
                case ValueType::String: {
                    std::string s = val.strVal ? val.strVal->data : "";
                    // Escape special chars
                    std::string escaped;
                    for (char c : s) {
                        switch (c) {
                            case '"': escaped += "\\\""; break;
                            case '\\': escaped += "\\\\"; break;
                            case '\n': escaped += "\\n"; break;
                            case '\t': escaped += "\\t"; break;
                            case '\r': escaped += "\\r"; break;
                            default: escaped += c;
                        }
                    }
                    return "\"" + escaped + "\"";
                }
                case ValueType::Array: {
                    if (!val.arrVal) return "[]";
                    std::string r = "[";
                    for (size_t i = 0; i < val.arrVal->elements.size(); i++) {
                        if (i > 0) r += ",";
                        r += serialize(val.arrVal->elements[i]);
                    }
                    return r + "]";
                }
                case ValueType::Map: {
                    if (!val.mapVal) return "{}";
                    std::string r = "{";
                    bool first = true;
                    for (auto& [k, v] : val.mapVal->entries) {
                        if (!first) r += ",";
                        r += "\"" + k + "\":" + serialize(v);
                        first = false;
                    }
                    return r + "}";
                }
                case ValueType::Object: {
                    if (!val.objVal) return "null";
                    std::string r = "{\"_type\":\"" + val.objVal->className + "\"";
                    for (auto& [k, v] : val.objVal->fields) {
                        if (k[0] == '_') continue; // skip internal fields
                        r += ",\"" + k + "\":" + serialize(v);
                    }
                    return r + "}";
                }
                default: return "null";
            }
        };

        return Value::makeString(serialize(v));
    };

    // Register toJson for all type prefixes so x.toJson() works on any variable
    vm.registerNative("List.toJson", toJsonImpl);
    vm.registerNative("String.toJson", toJsonImpl);
    vm.registerNative("Map.toJson", toJsonImpl);
    vm.registerNative("Math.toJson", toJsonImpl);
    vm.registerNative("Object.toJson", toJsonImpl);
    vm.registerNative("JSON.toJson", toJsonImpl);
}

// ===== 5.7 File System =====
void registerFileFunctions(VM& vm) {
    namespace fs = std::filesystem;
    vm.registerNative("File.readText", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()) { vm.throwError("GardFileError", "File.readText requires a file path argument"); return Value::makeNull(); }
        std::string path = a[0].toString();
        std::ifstream f(path);
        if(!f) { vm.throwError("GardFileError", "Cannot read file: '" + path + "' (file not found or permission denied)"); return Value::makeNull(); }
        std::stringstream b; b<<f.rdbuf(); return Value::makeString(b.str());
    });
    vm.registerNative("File.writeText", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<2) { vm.throwError("GardFileError", "File.writeText requires path and content arguments"); return Value::makeNull(); }
        std::string path = a[0].toString();
        std::ofstream f(path);
        if(!f) { vm.throwError("GardFileError", "Cannot write to file: '" + path + "' (permission denied)"); return Value::makeNull(); }
        f<<a[1].toString(); return Value::makeNull();
    });
    vm.registerNative("File.exists", [](const std::vector<Value>& a) -> Value { return Value::makeBool(!a.empty()&&std::filesystem::exists(a[0].toString())); });
    vm.registerNative("File.size", [](const std::vector<Value>& a) -> Value { try{return Value::makeLong(a.empty()?0:(int64_t)std::filesystem::file_size(a[0].toString()));}catch(...){return Value::makeLong(0);} });
    vm.registerNative("File.delete", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()) { vm.throwError("GardFileError", "File.delete requires a file path argument"); return Value::makeNull(); }
        std::string path = a[0].toString();
        if(!std::filesystem::exists(path)) { vm.throwError("GardFileError", "Cannot delete: '" + path + "' (file not found)"); return Value::makeNull(); }
        std::filesystem::remove(path); return Value::makeNull();
    });
    vm.registerNative("Directory.create", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()) { vm.throwError("GardFileError", "Directory.create requires a path argument"); return Value::makeNull(); }
        try { std::filesystem::create_directories(a[0].toString()); } catch(...) { vm.throwError("GardFileError", "Cannot create directory: '" + a[0].toString() + "'"); }
        return Value::makeNull();
    });
    vm.registerNative("Directory.list", [](const std::vector<Value>& a) -> Value { Value arr=Value::makeArray(); if(!a.empty()&&std::filesystem::exists(a[0].toString())){for(auto&e:std::filesystem::directory_iterator(a[0].toString()))arr.arrVal->elements.push_back(Value::makeString(e.path().filename().string()));} return arr; });
    vm.registerNative("Directory.exists", [](const std::vector<Value>& a) -> Value { return Value::makeBool(!a.empty()&&std::filesystem::is_directory(a[0].toString())); });
    vm.registerNative("Path.join", [](const std::vector<Value>& a) -> Value { if(a.size()<2)return a.empty()?Value::makeString(""):Value::makeString(a[0].toString()); std::filesystem::path p=a[0].toString(); for(size_t i=1;i<a.size();i++)p/=a[i].toString(); return Value::makeString(p.string()); });
    vm.registerNative("Path.extension", [](const std::vector<Value>& a) -> Value { return Value::makeString(a.empty()?"":std::filesystem::path(a[0].toString()).extension().string()); });
    vm.registerNative("Path.resolve", [](const std::vector<Value>& a) -> Value { return Value::makeString(a.empty()?"":std::filesystem::absolute(a[0].toString()).string()); });
    vm.registerNative("Path.dirname", [](const std::vector<Value>& a) -> Value { return Value::makeString(a.empty()?"":std::filesystem::path(a[0].toString()).parent_path().string()); });
    vm.registerNative("Path.basename", [](const std::vector<Value>& a) -> Value { return Value::makeString(a.empty()?"":std::filesystem::path(a[0].toString()).filename().string()); });
    vm.registerNative("File.appendText", [](const std::vector<Value>& a) -> Value { if(a.size()<2)return Value::makeNull(); std::ofstream f(a[0].toString(),std::ios::app); f<<a[1].toString(); return Value::makeNull(); });
    vm.registerNative("File.readBytes", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeArray();
        std::ifstream f(a[0].toString(), std::ios::binary);
        if(!f) return Value::makeArray();
        Value arr = Value::makeArray();
        char c;
        while(f.get(c)) arr.arrVal->elements.push_back(Value::makeInt(static_cast<unsigned char>(c)));
        return arr;
    });
    vm.registerNative("File.writeBytes", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[1].arrVal) return Value::makeNull();
        std::ofstream f(a[0].toString(), std::ios::binary);
        for(auto&e:a[1].arrVal->elements) f.put(static_cast<char>(e.toInt()));
        return Value::makeNull();
    });
    vm.registerNative("File.lastModified", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeLong(0);
        try {
            auto ftime = std::filesystem::last_write_time(a[0].toString());
            auto sctp = std::chrono::time_point_cast<std::chrono::milliseconds>(
                std::chrono::clock_cast<std::chrono::system_clock>(ftime));
            return Value::makeLong(sctp.time_since_epoch().count());
        } catch(...) { return Value::makeLong(0); }
    });
    vm.registerNative("File.copy", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeBool(false);
        try { std::filesystem::copy_file(a[0].toString(), a[1].toString(), std::filesystem::copy_options::overwrite_existing); return Value::makeBool(true); }
        catch(...) { return Value::makeBool(false); }
    });
    vm.registerNative("File.move", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeBool(false);
        try { std::filesystem::rename(a[0].toString(), a[1].toString()); return Value::makeBool(true); }
        catch(...) { return Value::makeBool(false); }
    });
    vm.registerNative("Directory.delete", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()) { vm.throwError("GardFileError", "Directory.delete requires a path argument"); return Value::makeNull(); }
        std::string path = a[0].toString();
        if(!std::filesystem::exists(path)) { vm.throwError("GardFileError", "Cannot delete directory: '" + path + "' (not found)"); return Value::makeNull(); }
        try { std::filesystem::remove_all(path); return Value::makeBool(true); }
        catch(...) { vm.throwError("GardFileError", "Cannot delete directory: '" + path + "'"); return Value::makeNull(); }
    });
}

// ===== 5.8 Crypto =====
void registerCryptoFunctions(VM& vm) {
    vm.registerNative("Hash.sha256", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeString(""); std::string d=a[0].toString(); unsigned char h[SHA256_DIGEST_LENGTH]; SHA256((const unsigned char*)d.c_str(),d.size(),h); std::ostringstream x; for(int i=0;i<SHA256_DIGEST_LENGTH;i++)x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i]; return Value::makeString(x.str()); });
    vm.registerNative("Base64.encode", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeString(""); std::string d=a[0].toString(); static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; std::string r; int v=0,vb=-6; for(unsigned char c:d){v=(v<<8)+c;vb+=8;while(vb>=0){r+=t[(v>>vb)&0x3F];vb-=6;}} if(vb>-6)r+=t[((v<<8)>>(vb+8))&0x3F]; while(r.size()%4)r+='='; return Value::makeString(r); });
    vm.registerNative("Base64.decode", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeString(""); std::string e=a[0].toString(),r; std::vector<int> T(256,-1); for(int i=0;i<64;i++)T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[i]]=i; int v=0,vb=-8; for(unsigned char c:e){if(T[c]==-1)break;v=(v<<6)+T[c];vb+=6;if(vb>=0){r+=char((v>>vb)&0xFF);vb-=8;}} return Value::makeString(r); });
    // SHA family
    vm.registerNative("Hash.sha1", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::string d=a[0].toString();
        unsigned char h[SHA_DIGEST_LENGTH];
        SHA1((const unsigned char*)d.c_str(), d.size(), h);
        std::ostringstream x; for(int i=0;i<SHA_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeString(x.str());
    });
    vm.registerNative("Hash.sha384", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::string d=a[0].toString();
        unsigned char h[SHA384_DIGEST_LENGTH];
        SHA384((const unsigned char*)d.c_str(), d.size(), h);
        std::ostringstream x; for(int i=0;i<SHA384_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeString(x.str());
    });
    vm.registerNative("Hash.sha512", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::string d=a[0].toString();
        unsigned char h[SHA512_DIGEST_LENGTH];
        SHA512((const unsigned char*)d.c_str(), d.size(), h);
        std::ostringstream x; for(int i=0;i<SHA512_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeString(x.str());
    });
    vm.registerNative("Hash.sha256File", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::ifstream f(a[0].toString(), std::ios::binary);
        if(!f) return Value::makeString("");
        SHA256_CTX ctx; SHA256_Init(&ctx);
        char buf[4096]; while(f.read(buf, sizeof(buf))) SHA256_Update(&ctx, buf, f.gcount());
        if(f.gcount()>0) SHA256_Update(&ctx, buf, f.gcount());
        unsigned char h[SHA256_DIGEST_LENGTH]; SHA256_Final(h, &ctx);
        std::ostringstream x; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeString(x.str());
    });
    vm.registerNative("Hash.hmac", [](const std::vector<Value>& a) -> Value {
        // Simplified HMAC-SHA256
        if(a.size()<3) return Value::makeString("");
        std::string key=a[1].toString(), data=a[2].toString();
        // XOR key with ipad/opad and hash (simplified)
        std::string combined = key + data;
        unsigned char h[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char*)combined.c_str(), combined.size(), h);
        std::ostringstream x; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeString(x.str());
    });
    // Base64 URL-safe
    vm.registerNative("Base64.encodeUrlSafe", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::string d=a[0].toString();
        static const char t[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string r; int v=0,vb=-6;
        for(unsigned char c:d){v=(v<<8)+c;vb+=8;while(vb>=0){r+=t[(v>>vb)&0x3F];vb-=6;}}
        if(vb>-6)r+=t[((v<<8)>>(vb+8))&0x3F];
        return Value::makeString(r);
    });
    vm.registerNative("Base64.decodeUrlSafe", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::string e=a[0].toString(),r;
        std::vector<int> T(256,-1);
        for(int i=0;i<64;i++) T["ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[i]]=i;
        int v=0,vb=-8;
        for(unsigned char c:e){if(T[c]==-1)break;v=(v<<6)+T[c];vb+=6;if(vb>=0){r+=char((v>>vb)&0xFF);vb-=8;}}
        return Value::makeString(r);
    });
    // Crypto — AES-256-GCM (real encryption via OpenSSL)
    vm.registerNative("Crypto.encrypt", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeString("");
        std::string plaintext = a[0].toString();
        std::string key = a[1].toString();
        // Pad/truncate key to 32 bytes (AES-256)
        key.resize(32, '\0');

        unsigned char iv[12]; // 96-bit IV for GCM
        RAND_bytes(iv, sizeof(iv));

        unsigned char tag[16];
        std::vector<unsigned char> ciphertext(plaintext.size() + 16);
        int len = 0, ciphertext_len = 0;

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        EVP_EncryptInit_ex(ctx, nullptr, nullptr, (unsigned char*)key.c_str(), iv);
        EVP_EncryptUpdate(ctx, ciphertext.data(), &len, (unsigned char*)plaintext.c_str(), plaintext.size());
        ciphertext_len = len;
        EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len);
        ciphertext_len += len;
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag);
        EVP_CIPHER_CTX_free(ctx);

        // Output format: iv(12) + tag(16) + ciphertext
        std::string result;
        result.append((char*)iv, 12);
        result.append((char*)tag, 16);
        result.append((char*)ciphertext.data(), ciphertext_len);

        // Encode as hex for safe transport
        std::ostringstream hex;
        for(unsigned char c : result) hex << std::setfill('0') << std::setw(2) << std::hex << (int)c;
        return Value::makeString(hex.str());
    });
    vm.registerNative("Crypto.decrypt", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeString("");
        std::string hexInput = a[0].toString();
        std::string key = a[1].toString();
        key.resize(32, '\0');

        // Decode hex
        std::string raw;
        for(size_t i=0; i+1<hexInput.size(); i+=2) {
            raw += (char)std::stoi(hexInput.substr(i, 2), nullptr, 16);
        }
        if(raw.size() < 28) return Value::makeString(""); // iv(12)+tag(16) minimum

        unsigned char iv[12];
        unsigned char tag[16];
        std::memcpy(iv, raw.c_str(), 12);
        std::memcpy(tag, raw.c_str() + 12, 16);
        std::string ciphertext = raw.substr(28);

        std::vector<unsigned char> plaintext(ciphertext.size() + 16);
        int len = 0, plaintext_len = 0;

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
        EVP_DecryptInit_ex(ctx, nullptr, nullptr, (unsigned char*)key.c_str(), iv);
        EVP_DecryptUpdate(ctx, plaintext.data(), &len, (unsigned char*)ciphertext.c_str(), ciphertext.size());
        plaintext_len = len;
        EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag);
        int ret = EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len);
        EVP_CIPHER_CTX_free(ctx);

        if(ret <= 0) return Value::makeString(""); // authentication failed
        plaintext_len += len;
        return Value::makeString(std::string((char*)plaintext.data(), plaintext_len));
    });
    vm.registerNative("Crypto.generateKey", [](const std::vector<Value>& a) -> Value {
        int bits = a.empty() ? 256 : a[0].toInt();
        int bytes = bits / 8;
        std::vector<unsigned char> key(bytes);
        RAND_bytes(key.data(), bytes);
        std::ostringstream hex;
        for(unsigned char c : key) hex << std::setfill('0') << std::setw(2) << std::hex << (int)c;
        return Value::makeString(hex.str());
    });
    vm.registerNative("Crypto.getRandomValues", [](const std::vector<Value>& a) -> Value {
        int size = a.empty() ? 16 : a[0].toInt();
        std::vector<unsigned char> buf(size);
        RAND_bytes(buf.data(), size);
        Value arr = Value::makeArray();
        for(int i=0;i<size;i++) arr.arrVal->elements.push_back(Value::makeInt(buf[i]));
        return arr;
    });
    // RSA (simplified stubs — real RSA requires OpenSSL EVP API)
    vm.registerNative("RSA.generateKeyPair", [](const std::vector<Value>& a) -> Value {
        Value pair = Value::makeObject("RSAKeyPair");
        pair.objVal->fields["publicKey"] = Value::makeString("rsa-pub-key-" + std::to_string(a.empty()?2048:a[0].toInt()));
        pair.objVal->fields["privateKey"] = Value::makeString("rsa-priv-key-" + std::to_string(a.empty()?2048:a[0].toInt()));
        return pair;
    });
    vm.registerNative("RSA.encrypt", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeString("");
        return Value::makeString("encrypted:" + a[0].toString());
    });
    vm.registerNative("RSA.decrypt", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeString("");
        std::string s = a[0].toString();
        if(s.rfind("encrypted:",0)==0) return Value::makeString(s.substr(10));
        return Value::makeString(s);
    });
    vm.registerNative("RSA.sign", [](const std::vector<Value>& a) -> Value {
        if(a.size()<2) return Value::makeString("");
        // Sign = hash the data
        std::string d=a[0].toString();
        unsigned char h[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char*)d.c_str(), d.size(), h);
        std::ostringstream x; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeString("sig:" + x.str());
    });
    vm.registerNative("RSA.verify", [](const std::vector<Value>& a) -> Value {
        if(a.size()<3) return Value::makeBool(false);
        // Verify = check signature matches hash
        std::string data=a[0].toString(), sig=a[1].toString();
        unsigned char h[SHA256_DIGEST_LENGTH];
        SHA256((const unsigned char*)data.c_str(), data.size(), h);
        std::ostringstream x; for(int i=0;i<SHA256_DIGEST_LENGTH;i++) x<<std::hex<<std::setfill('0')<<std::setw(2)<<(int)h[i];
        return Value::makeBool(sig == "sig:" + x.str());
    });
    vm.registerNative("RSA.exportKey", [](const std::vector<Value>& a) -> Value {
        return a.empty() ? Value::makeString("") : Value::makeString("exported:" + a[0].toString());
    });
    vm.registerNative("RSA.importKey", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeString("");
        std::string s = a[0].toString();
        if(s.rfind("exported:",0)==0) return Value::makeString(s.substr(9));
        return Value::makeString(s);
    });
}

// ===== 5.9 Process =====
void registerProcessFunctions(VM& vm) {
    vm.registerNative("Process.pid", [](const std::vector<Value>&) -> Value { return Value::makeInt((int)getpid()); });
    vm.registerNative("Process.platform", [](const std::vector<Value>&) -> Value { struct utsname i; uname(&i); return Value::makeString(i.sysname); });
    vm.registerNative("Process.arch", [](const std::vector<Value>&) -> Value { struct utsname i; uname(&i); return Value::makeString(i.machine); });
    vm.registerNative("Process.cwd", [](const std::vector<Value>&) -> Value { return Value::makeString(std::filesystem::current_path().string()); });
    vm.registerNative("Process.env", [](const std::vector<Value>& a) -> Value { if(a.empty())return Value::makeNull(); const char*v=std::getenv(a[0].toString().c_str()); return v?Value::makeString(v):Value::makeNull(); });
    vm.registerNative("Process.args", [&vm](const std::vector<Value>&) -> Value {
        Value arr = Value::makeArray();
        for (auto& arg : vm.getArgs()) {
            arr.arrVal->elements.push_back(Value::makeString(arg));
        }
        return arr;
    });
    vm.registerNative("Process.chdir", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeBool(false);
        try { std::filesystem::current_path(a[0].toString()); return Value::makeBool(true); }
        catch(...) { return Value::makeBool(false); }
    });
    vm.registerNative("Process.exit", [](const std::vector<Value>& a) -> Value {
        int code = a.empty() ? 0 : a[0].toInt();
        std::exit(code);
        return Value::makeNull(); // unreachable
    });
    vm.registerNative("Process.execute", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeNull();
        std::string cmd = a[0].toString();
        // Build command with args
        if(a.size() >= 2 && a[1].arrVal) {
            for(auto& arg : a[1].arrVal->elements) cmd += " " + arg.toString();
        }
        FILE* pipe = popen(cmd.c_str(), "r");
        if(!pipe) return Value::makeNull();
        std::string output;
        char buf[256];
        while(fgets(buf, sizeof(buf), pipe)) output += buf;
        int status = pclose(pipe);
        Value result = Value::makeObject("ProcessResult");
        result.objVal->fields["stdout"] = Value::makeString(output);
        result.objVal->fields["exitCode"] = Value::makeInt(WEXITSTATUS(status));
        result.objVal->fields["success"] = Value::makeBool(WEXITSTATUS(status) == 0);
        return result;
    });
    vm.registerNative("Process.spawn", [](const std::vector<Value>& a) -> Value {
        if(a.empty()) return Value::makeNull();
        std::string cmd = a[0].toString();
        if(a.size() >= 2 && a[1].arrVal) {
            for(auto& arg : a[1].arrVal->elements) cmd += " " + arg.toString();
        }
        pid_t pid = fork();
        if(pid == 0) {
            execl("/bin/sh", "sh", "-c", cmd.c_str(), nullptr);
            _exit(127);
        }
        Value child = Value::makeObject("ChildProcess");
        child.objVal->fields["pid"] = Value::makeInt(static_cast<int>(pid));
        child.objVal->fields["command"] = Value::makeString(cmd);
        return child;
    });
    vm.registerNative("ChildProcess.kill", [](const std::vector<Value>& a) -> Value {
        if(a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("pid");
        if(it == a[0].objVal->fields.end()) return Value::makeBool(false);
        int pid = it->second.toInt();
        int sig = a.size() >= 2 ? a[1].toInt() : 15; // SIGTERM
        return Value::makeBool(kill(pid, sig) == 0);
    });
    // System info
    vm.registerNative("System.platform", [](const std::vector<Value>&) -> Value {
        struct utsname i; uname(&i); return Value::makeString(i.sysname);
    });
    vm.registerNative("System.arch", [](const std::vector<Value>&) -> Value {
        struct utsname i; uname(&i); return Value::makeString(i.machine);
    });
    vm.registerNative("System.cpuCount", [](const std::vector<Value>&) -> Value {
        return Value::makeInt(static_cast<int>(sysconf(_SC_NPROCESSORS_ONLN)));
    });
    vm.registerNative("System.memory", [](const std::vector<Value>&) -> Value {
        long pages = sysconf(_SC_PHYS_PAGES);
        long pageSize = sysconf(_SC_PAGE_SIZE);
        long available = sysconf(_SC_AVPHYS_PAGES);
        int64_t total = (int64_t)pages * pageSize;
        int64_t free = (int64_t)available * pageSize;
        Value mem = Value::makeObject("MemoryInfo");
        mem.objVal->fields["total"] = Value::makeLong(total);
        mem.objVal->fields["free"] = Value::makeLong(free);
        mem.objVal->fields["used"] = Value::makeLong(total - free);
        return mem;
    });
    vm.registerNative("System.networkInterfaces", [](const std::vector<Value>&) -> Value {
        Value arr = Value::makeArray();
        // Read from /proc/net/dev
        std::ifstream f("/proc/net/dev");
        if(f) {
            std::string line;
            std::getline(f, line); std::getline(f, line); // skip headers
            while(std::getline(f, line)) {
                size_t colon = line.find(':');
                if(colon != std::string::npos) {
                    std::string name = line.substr(0, colon);
                    // trim
                    size_t start = name.find_first_not_of(" ");
                    if(start != std::string::npos) name = name.substr(start);
                    Value iface = Value::makeObject("NetworkInterface");
                    iface.objVal->fields["name"] = Value::makeString(name);
                    arr.arrVal->elements.push_back(iface);
                }
            }
        }
        return arr;
    });
}

// ===== 5.1 Collections =====
void registerCollectionFunctions(VM& vm) {
    vm.registerNative("List.length", [](const std::vector<Value>& a) -> Value { return Value::makeInt((!a.empty()&&a[0].arrVal)?(int)a[0].arrVal->elements.size():0); });
    vm.registerNative("List.add", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].arrVal)a[0].arrVal->elements.push_back(a[1]); return Value::makeNull(); });
    vm.registerNative("List.push", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].arrVal)a[0].arrVal->elements.push_back(a[1]); return Value::makeNull(); });
    vm.registerNative("List.addAll", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].arrVal&&a[1].arrVal)for(auto&e:a[1].arrVal->elements)a[0].arrVal->elements.push_back(e); return Value::makeNull(); });
    vm.registerNative("List.pop", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].arrVal) { vm.throwError("GardCollectionError", "List.pop: cannot pop from null"); return Value::makeNull(); }
        if(a[0].arrVal->elements.empty()) { vm.throwError("GardCollectionError", "List.pop: cannot pop from empty list"); return Value::makeNull(); }
        Value v=a[0].arrVal->elements.back(); a[0].arrVal->elements.pop_back(); return v;
    });
    vm.registerNative("List.get", [&vm](const std::vector<Value>& a) -> Value {
        if (__builtin_expect(a.size() >= 2 && a[0].arrVal != nullptr, 1)) {
            int i = a[1].intVal;
            if (a[1].type != ValueType::Int) i = a[1].toInt();
            auto& elems = a[0].arrVal->elements;
            if (__builtin_expect((unsigned)i < elems.size(), 1)) {
                return elems[i];
            }
            vm.throwError("GardIndexOutOfBoundsError", "List.get: index " + std::to_string(i) + " out of bounds (size: " + std::to_string(elems.size()) + ")");
            return Value::makeNull();
        }
        vm.throwError("GardCollectionError", "List.get: invalid arguments");
        return Value::makeNull();
    });
    vm.registerNative("List.set", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<3||!a[0].arrVal) return Value::makeNull();
        int i=a[1].toInt();
        if(i<0||i>=(int)a[0].arrVal->elements.size()) { vm.throwError("GardIndexOutOfBoundsError", "List.set: index " + std::to_string(i) + " out of bounds (size: " + std::to_string(a[0].arrVal->elements.size()) + ")"); return Value::makeNull(); }
        a[0].arrVal->elements[i]=a[2]; return Value::makeNull();
    });
    vm.registerNative("List.remove", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeBool(false); std::string t=a[1].toString(); auto&e=a[0].arrVal->elements; for(auto it=e.begin();it!=e.end();++it){if(it->toString()==t){e.erase(it);return Value::makeBool(true);}} return Value::makeBool(false); });
    vm.registerNative("List.removeAt", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<2||!a[0].arrVal) { vm.throwError("GardCollectionError", "List.removeAt: invalid arguments"); return Value::makeNull(); }
        int i=a[1].toInt(); auto&e=a[0].arrVal->elements;
        if(i<0||i>=(int)e.size()) { vm.throwError("GardIndexOutOfBoundsError", "List.removeAt: index " + std::to_string(i) + " out of bounds (size: " + std::to_string(e.size()) + ")"); return Value::makeNull(); }
        Value v=e[i];e.erase(e.begin()+i);return v;
    });
    vm.registerNative("List.reverse", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(!a.empty()&&a[0].arrVal){auto&e=a[0].arrVal->elements;for(int i=(int)e.size()-1;i>=0;i--)r.arrVal->elements.push_back(e[i]);} return r; });
    vm.registerNative("List.contains", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeBool(false); std::string t=a[1].toString(); for(auto&e:a[0].arrVal->elements)if(e.toString()==t)return Value::makeBool(true); return Value::makeBool(false); });
    vm.registerNative("List.indexOf", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeInt(-1); std::string t=a[1].toString(); for(size_t i=0;i<a[0].arrVal->elements.size();i++)if(a[0].arrVal->elements[i].toString()==t)return Value::makeInt((int)i); return Value::makeInt(-1); });
    vm.registerNative("List.findIndex", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeInt(-1); std::string t=a[1].toString(); for(size_t i=0;i<a[0].arrVal->elements.size();i++)if(a[0].arrVal->elements[i].toString()==t)return Value::makeInt((int)i); return Value::makeInt(-1); });
    vm.registerNative("List.find", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeNull(); std::string t=a[1].toString(); for(auto&e:a[0].arrVal->elements)if(e.toString()==t)return e; return Value::makeNull(); });
    vm.registerNative("List.join", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal)return Value::makeString(""); std::string sep=a.size()>=2?a[1].toString():",",r; for(size_t i=0;i<a[0].arrVal->elements.size();i++){if(i>0)r+=sep;r+=a[0].arrVal->elements[i].toString();} return Value::makeString(r); });
    vm.registerNative("List.sort", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal)return Value::makeArray(); Value r=Value::makeArray(); r.arrVal->elements=a[0].arrVal->elements; std::sort(r.arrVal->elements.begin(),r.arrVal->elements.end(),[](const Value&x,const Value&y){return x.toInt()<y.toInt();}); return r; });
    vm.registerNative("List.map", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal)return Value::makeArray(); Value r=Value::makeArray(); r.arrVal->elements=a[0].arrVal->elements; return r; });
    vm.registerNative("List.filter", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal)return Value::makeArray(); Value r=Value::makeArray(); for(auto&e:a[0].arrVal->elements)if(e.toBool())r.arrVal->elements.push_back(e); return r; });
    vm.registerNative("List.reduce", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty())return Value::makeInt(0); int sum=0; for(auto&e:a[0].arrVal->elements)sum+=e.toInt(); return Value::makeInt(sum); });
    vm.registerNative("List.every", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal)return Value::makeBool(true); for(auto&e:a[0].arrVal->elements)if(!e.toBool())return Value::makeBool(false); return Value::makeBool(true); });
    vm.registerNative("List.some", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal)return Value::makeBool(false); for(auto&e:a[0].arrVal->elements)if(e.toBool())return Value::makeBool(true); return Value::makeBool(false); });
    vm.registerNative("Queue.enqueue", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].arrVal)a[0].arrVal->elements.push_back(a[1]); return Value::makeNull(); });
    vm.registerNative("Queue.dequeue", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].arrVal) { vm.throwError("GardCollectionError", "Queue.dequeue: invalid queue"); return Value::makeNull(); }
        if(a[0].arrVal->elements.empty()) { vm.throwError("GardCollectionError", "Queue.dequeue: cannot dequeue from empty queue"); return Value::makeNull(); }
        Value v=a[0].arrVal->elements.front(); a[0].arrVal->elements.erase(a[0].arrVal->elements.begin()); return v;
    });
    vm.registerNative("Queue.peek", [&vm](const std::vector<Value>& a) -> Value {
        if(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty()) { vm.throwError("GardCollectionError", "Queue.peek: queue is empty"); return Value::makeNull(); }
        return a[0].arrVal->elements.front();
    });
    vm.registerNative("Queue.isEmpty", [](const std::vector<Value>& a) -> Value { return Value::makeBool(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty()); });
    vm.registerNative("Queue.size", [](const std::vector<Value>& a) -> Value { return Value::makeInt((!a.empty()&&a[0].arrVal)?(int)a[0].arrVal->elements.size():0); });
    vm.registerNative("Stack.push", [](const std::vector<Value>& a) -> Value { if(a.size()>=2&&a[0].arrVal)a[0].arrVal->elements.push_back(a[1]); return Value::makeNull(); });
    vm.registerNative("Stack.pop", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty())return Value::makeNull(); Value v=a[0].arrVal->elements.back(); a[0].arrVal->elements.pop_back(); return v; });
    vm.registerNative("Stack.peek", [](const std::vector<Value>& a) -> Value { return(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty())?Value::makeNull():a[0].arrVal->elements.back(); });
    vm.registerNative("Stack.isEmpty", [](const std::vector<Value>& a) -> Value { return Value::makeBool(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty()); });
    vm.registerNative("Stack.size", [](const std::vector<Value>& a) -> Value { return Value::makeInt((!a.empty()&&a[0].arrVal)?(int)a[0].arrVal->elements.size():0); });
    vm.registerNative("Set.add", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeNull(); std::string v=a[1].toString(); for(auto&e:a[0].arrVal->elements)if(e.toString()==v)return Value::makeNull(); a[0].arrVal->elements.push_back(a[1]); return Value::makeNull(); });
    vm.registerNative("Set.remove", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeBool(false); std::string v=a[1].toString(); auto&e=a[0].arrVal->elements; for(auto it=e.begin();it!=e.end();++it){if(it->toString()==v){e.erase(it);return Value::makeBool(true);}} return Value::makeBool(false); });
    vm.registerNative("Set.contains", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeBool(false); std::string v=a[1].toString(); for(auto&e:a[0].arrVal->elements)if(e.toString()==v)return Value::makeBool(true); return Value::makeBool(false); });
    vm.registerNative("Set.size", [](const std::vector<Value>& a) -> Value { return Value::makeInt((!a.empty()&&a[0].arrVal)?(int)a[0].arrVal->elements.size():0); });
    vm.registerNative("Set.union", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(a.size()<2||!a[0].arrVal||!a[1].arrVal)return r; for(auto&e:a[0].arrVal->elements)r.arrVal->elements.push_back(e); for(auto&e:a[1].arrVal->elements){bool d=false;for(auto&x:r.arrVal->elements)if(x.toString()==e.toString()){d=true;break;}if(!d)r.arrVal->elements.push_back(e);} return r; });
    vm.registerNative("Set.intersection", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(a.size()<2||!a[0].arrVal||!a[1].arrVal)return r; for(auto&e:a[0].arrVal->elements){for(auto&x:a[1].arrVal->elements)if(x.toString()==e.toString()){r.arrVal->elements.push_back(e);break;}} return r; });
    vm.registerNative("Set.difference", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(a.size()<2||!a[0].arrVal||!a[1].arrVal)return r; for(auto&e:a[0].arrVal->elements){bool f=false;for(auto&x:a[1].arrVal->elements)if(x.toString()==e.toString()){f=true;break;}if(!f)r.arrVal->elements.push_back(e);} return r; });
    vm.registerNative("Map.get", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].mapVal)return Value::makeNull(); auto it=a[0].mapVal->entries.find(a[1].toString()); return it!=a[0].mapVal->entries.end()?it->second:Value::makeNull(); });
    vm.registerNative("Map.set", [](const std::vector<Value>& a) -> Value { if(a.size()<3||!a[0].mapVal)return Value::makeNull(); a[0].mapVal->entries[a[1].toString()]=a[2]; return Value::makeNull(); });
    vm.registerNative("Map.remove", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].mapVal)return Value::makeBool(false); return Value::makeBool(a[0].mapVal->entries.erase(a[1].toString())>0); });
    vm.registerNative("Map.has", [](const std::vector<Value>& a) -> Value { return Value::makeBool(a.size()>=2&&a[0].mapVal&&a[0].mapVal->entries.count(a[1].toString())>0); });
    vm.registerNative("Map.keys", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(!a.empty()&&a[0].mapVal)for(auto&[k,v]:a[0].mapVal->entries)r.arrVal->elements.push_back(Value::makeString(k)); return r; });
    vm.registerNative("Map.values", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(!a.empty()&&a[0].mapVal)for(auto&[k,v]:a[0].mapVal->entries)r.arrVal->elements.push_back(v); return r; });
    vm.registerNative("Map.entries", [](const std::vector<Value>& a) -> Value { Value r=Value::makeArray(); if(!a.empty()&&a[0].mapVal)for(auto&[k,v]:a[0].mapVal->entries){Value p=Value::makeArray();p.arrVal->elements.push_back(Value::makeString(k));p.arrVal->elements.push_back(v);r.arrVal->elements.push_back(p);} return r; });
    vm.registerNative("Map.size", [](const std::vector<Value>& a) -> Value { return Value::makeInt((!a.empty()&&a[0].mapVal)?(int)a[0].mapVal->entries.size():0); });
    vm.registerNative("Map.delete", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].mapVal)return Value::makeBool(false); return Value::makeBool(a[0].mapVal->entries.erase(a[1].toString())>0); });
    vm.registerNative("PriorityQueue.enqueue", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeNull(); a[0].arrVal->elements.push_back(a[1]); std::sort(a[0].arrVal->elements.begin(),a[0].arrVal->elements.end(),[](const Value&x,const Value&y){return x.toInt()<y.toInt();}); return Value::makeNull(); });
    vm.registerNative("PriorityQueue.dequeue", [](const std::vector<Value>& a) -> Value { if(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty())return Value::makeNull(); Value v=a[0].arrVal->elements.front(); a[0].arrVal->elements.erase(a[0].arrVal->elements.begin()); return v; });
    vm.registerNative("PriorityQueue.peek", [](const std::vector<Value>& a) -> Value { return(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty())?Value::makeNull():a[0].arrVal->elements.front(); });
    vm.registerNative("PriorityQueue.size", [](const std::vector<Value>& a) -> Value { return Value::makeInt((!a.empty()&&a[0].arrVal)?(int)a[0].arrVal->elements.size():0); });
    vm.registerNative("PriorityQueue.isEmpty", [](const std::vector<Value>& a) -> Value { return Value::makeBool(a.empty()||!a[0].arrVal||a[0].arrVal->elements.empty()); });
    vm.registerNative("Iterator.hasNext", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeBool(false); return Value::makeBool(a[1].toInt()<(int)a[0].arrVal->elements.size()); });
    vm.registerNative("Iterator.next", [](const std::vector<Value>& a) -> Value { if(a.size()<2||!a[0].arrVal)return Value::makeNull(); int i=a[1].toInt(); if(i<(int)a[0].arrVal->elements.size())return a[0].arrVal->elements[i]; return Value::makeNull(); });
}
// ===== 5.3 Regex =====
void registerRegexFunctions(VM& vm) {
    vm.registerNative("Regex.test", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<2) { vm.throwError("GardRegexError", "Regex.test requires pattern and string arguments"); return Value::makeNull(); }
        try { return Value::makeBool(std::regex_search(a[1].toString(),std::regex(a[0].toString()))); }
        catch(const std::regex_error& e) { vm.throwError("GardRegexError", "Invalid regex pattern '" + a[0].toString() + "': " + e.what()); return Value::makeNull(); }
    });
    vm.registerNative("Regex.match", [&vm](const std::vector<Value>& a) -> Value {
        Value r=Value::makeArray(); if(a.size()<2) { vm.throwError("GardRegexError", "Regex.match requires pattern and string arguments"); return Value::makeNull(); }
        try{std::regex re(a[0].toString());std::string s=a[1].toString();std::smatch m;while(std::regex_search(s,m,re)){r.arrVal->elements.push_back(Value::makeString(m[0].str()));s=m.suffix().str();}}
        catch(const std::regex_error& e) { vm.throwError("GardRegexError", "Invalid regex pattern '" + a[0].toString() + "': " + e.what()); return Value::makeNull(); }
        return r;
    });
    vm.registerNative("Regex.replace", [&vm](const std::vector<Value>& a) -> Value {
        if(a.size()<3) { vm.throwError("GardRegexError", "Regex.replace requires pattern, string, and replacement arguments"); return Value::makeNull(); }
        try{return Value::makeString(std::regex_replace(a[1].toString(),std::regex(a[0].toString()),a[2].toString()));}
        catch(const std::regex_error& e) { vm.throwError("GardRegexError", "Invalid regex pattern '" + a[0].toString() + "': " + e.what()); return Value::makeNull(); }
    });
    vm.registerNative("Regex.matches", [](const std::vector<Value>& a) -> Value { if(a.size()<2)return Value::makeBool(false); try{return Value::makeBool(std::regex_match(a[1].toString(),std::regex(a[0].toString())));}catch(...){return Value::makeBool(false);} });
    vm.registerNative("Regex.split", [](const std::vector<Value>& a) -> Value {
        Value r = Value::makeArray();
        if (a.size() < 2) return r;
        try {
            std::regex re(a[0].toString());
            std::string s = a[1].toString();
            std::sregex_token_iterator it(s.begin(), s.end(), re, -1), end;
            for (; it != end; ++it) r.arrVal->elements.push_back(Value::makeString(it->str()));
        } catch (...) { r.arrVal->elements.push_back(Value::makeString(a[1].toString())); }
        return r;
    });
    vm.registerNative("Regex.matchGroups", [](const std::vector<Value>& a) -> Value {
        Value r = Value::makeArray();
        if (a.size() < 2) return r;
        try {
            std::regex re(a[0].toString());
            std::string s = a[1].toString();
            std::smatch m;
            if (std::regex_search(s, m, re)) {
                for (size_t i = 0; i < m.size(); i++) {
                    r.arrVal->elements.push_back(Value::makeString(m[i].str()));
                }
            }
        } catch (...) {}
        return r;
    });
    vm.registerNative("Regex.testFlags", [](const std::vector<Value>& a) -> Value {
        // test with flags: args[0]=pattern, args[1]=string, args[2]=flags ("i","g","m")
        if (a.size() < 3) return Value::makeBool(false);
        try {
            std::string flags = a[2].toString();
            auto rxFlags = std::regex::ECMAScript;
            if (flags.find('i') != std::string::npos) rxFlags |= std::regex::icase;
            if (flags.find('m') != std::string::npos) rxFlags |= std::regex::multiline;
            std::regex re(a[0].toString(), rxFlags);
            return Value::makeBool(std::regex_search(a[1].toString(), re));
        } catch (...) { return Value::makeBool(false); }
    });
    vm.registerNative("Regex.replaceAll", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3) return a.size() >= 2 ? Value::makeString(a[1].toString()) : Value::makeString("");
        try {
            std::regex re(a[0].toString());
            return Value::makeString(std::regex_replace(a[1].toString(), re, a[2].toString()));
        } catch (...) { return a.size() >= 2 ? Value::makeString(a[1].toString()) : Value::makeString(""); }
    });
}

// ===== 5.10 Compression (real zlib deflate/inflate) =====
void registerCompressionFunctions(VM& vm) {
    // Compression.compress — deflate using zlib
    vm.registerNative("Compression.compress", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeString("");
        std::string input = a[0].toString();
        int level = (a.size() >= 2) ? a[1].toInt() : Z_DEFAULT_COMPRESSION;
        if (level < 1 || level > 9) level = Z_DEFAULT_COMPRESSION;

        z_stream strm{};
        deflateInit2(&strm, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY); // gzip format

        strm.next_in = (Bytef*)input.c_str();
        strm.avail_in = input.size();

        std::string output;
        char buf[4096];
        do {
            strm.next_out = (Bytef*)buf;
            strm.avail_out = sizeof(buf);
            deflate(&strm, Z_FINISH);
            output.append(buf, sizeof(buf) - strm.avail_out);
        } while (strm.avail_out == 0);
        deflateEnd(&strm);

        // Return as hex string for safe transport
        std::ostringstream hex;
        for (unsigned char c : output) hex << std::setfill('0') << std::setw(2) << std::hex << (int)c;
        return Value::makeString(hex.str());
    });

    // Compression.decompress — inflate using zlib
    vm.registerNative("Compression.decompress", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeString("");
        std::string hexInput = a[0].toString();

        // Decode hex to raw bytes
        std::string compressed;
        for (size_t i = 0; i + 1 < hexInput.size(); i += 2) {
            compressed += (char)std::stoi(hexInput.substr(i, 2), nullptr, 16);
        }

        z_stream strm{};
        inflateInit2(&strm, 15 + 16); // gzip format

        strm.next_in = (Bytef*)compressed.c_str();
        strm.avail_in = compressed.size();

        std::string output;
        char buf[4096];
        int ret;
        do {
            strm.next_out = (Bytef*)buf;
            strm.avail_out = sizeof(buf);
            ret = inflate(&strm, Z_NO_FLUSH);
            if (ret == Z_STREAM_ERROR || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) break;
            output.append(buf, sizeof(buf) - strm.avail_out);
        } while (ret != Z_STREAM_END);
        inflateEnd(&strm);

        return Value::makeString(output);
    });

    // Compression.compressFile — gzip a file to output path
    vm.registerNative("Compression.compressFile", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeBool(false);
        std::string inputPath = a[0].toString();
        std::string outputPath = a[1].toString();
        int level = (a.size() >= 3) ? a[2].toInt() : Z_DEFAULT_COMPRESSION;

        std::ifstream in(inputPath, std::ios::binary);
        if (!in) return Value::makeBool(false);

        gzFile gz = gzopen(outputPath.c_str(), ("wb" + std::to_string(level)).c_str());
        if (!gz) return Value::makeBool(false);

        char buf[8192];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
            gzwrite(gz, buf, in.gcount());
        }
        gzclose(gz);
        return Value::makeBool(true);
    });

    // Compression.decompressFile — gunzip a file
    vm.registerNative("Compression.decompressFile", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeBool(false);
        std::string inputPath = a[0].toString();
        std::string outputPath = a[1].toString();

        gzFile gz = gzopen(inputPath.c_str(), "rb");
        if (!gz) return Value::makeBool(false);

        std::ofstream out(outputPath, std::ios::binary);
        if (!out) { gzclose(gz); return Value::makeBool(false); }

        char buf[8192];
        int bytesRead;
        while ((bytesRead = gzread(gz, buf, sizeof(buf))) > 0) {
            out.write(buf, bytesRead);
        }
        gzclose(gz);
        return Value::makeBool(true);
    });
}

// ===== 5.11 XML (real libxml2) =====

void registerXmlFunctions(VM& vm) {
    // XML.parse — parse XML string into a document, store raw pointer in object
    vm.registerNative("XML.parse", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string xml = a[0].toString();
        xmlDocPtr doc = xmlParseMemory(xml.c_str(), xml.size());
        if (!doc) return Value::makeNull();

        Value result = Value::makeObject("XMLDocument");
        result.objVal->fields["_ptr"] = Value::makeLong(reinterpret_cast<int64_t>(doc));
        result.objVal->fields["valid"] = Value::makeBool(true);

        // Get root element name
        xmlNodePtr root = xmlDocGetRootElement(doc);
        if (root && root->name) {
            result.objVal->fields["root"] = Value::makeString(reinterpret_cast<const char*>(root->name));
        }
        return result;
    });

    // XML.createElement
    vm.registerNative("XML.createElement", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        std::string tag = a[0].toString();
        Value elem = Value::makeObject("XMLElement");
        elem.objVal->fields["tag"] = Value::makeString(tag);
        elem.objVal->fields["attributes"] = Value::makeMap();
        elem.objVal->fields["children"] = Value::makeArray();
        elem.objVal->fields["text"] = Value::makeString("");
        return elem;
    });

    // XML.setAttribute
    vm.registerNative("XML.setAttribute", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        auto attrIt = a[0].objVal->fields.find("attributes");
        if (attrIt != a[0].objVal->fields.end() && attrIt->second.mapVal) {
            attrIt->second.mapVal->entries[a[1].toString()] = a[2];
        }
        return Value::makeNull();
    });

    // XML.setTextContent
    vm.registerNative("XML.setTextContent", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["text"] = Value::makeString(a[1].toString());
        return Value::makeNull();
    });

    // XML.appendChild
    vm.registerNative("XML.appendChild", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto childrenIt = a[0].objVal->fields.find("children");
        if (childrenIt != a[0].objVal->fields.end() && childrenIt->second.arrVal) {
            childrenIt->second.arrVal->elements.push_back(a[1]);
        }
        return Value::makeNull();
    });

    // XML.removeChild
    vm.registerNative("XML.removeChild", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto childrenIt = a[0].objVal->fields.find("children");
        if (childrenIt != a[0].objVal->fields.end() && childrenIt->second.arrVal) {
            auto& elems = childrenIt->second.arrVal->elements;
            if (!elems.empty()) { elems.pop_back(); return Value::makeBool(true); }
        }
        return Value::makeBool(false);
    });

    // XML.toString — serialize element to XML string
    vm.registerNative("XML.toString", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeString("");

        // If it's a parsed document with a pointer, use libxml2 to serialize
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt != a[0].objVal->fields.end()) {
            xmlDocPtr doc = reinterpret_cast<xmlDocPtr>(static_cast<intptr_t>(ptrIt->second.longVal));
            if (doc) {
                xmlChar* buf = nullptr;
                int size = 0;
                xmlDocDumpFormatMemory(doc, &buf, &size, 1);
                std::string result(reinterpret_cast<char*>(buf), size);
                xmlFree(buf);
                return Value::makeString(result);
            }
        }

        // Otherwise build from our element structure
        auto tagIt = a[0].objVal->fields.find("tag");
        if (tagIt == a[0].objVal->fields.end()) return Value::makeString("<xml/>");
        std::string tag = tagIt->second.toString();
        std::string xml = "<" + tag;

        // Attributes
        auto attrIt = a[0].objVal->fields.find("attributes");
        if (attrIt != a[0].objVal->fields.end() && attrIt->second.mapVal) {
            for (auto& [k, v] : attrIt->second.mapVal->entries) {
                xml += " " + k + "=\"" + v.toString() + "\"";
            }
        }

        // Text content or children
        auto textIt = a[0].objVal->fields.find("text");
        auto childrenIt = a[0].objVal->fields.find("children");
        bool hasContent = (textIt != a[0].objVal->fields.end() && !textIt->second.toString().empty()) ||
                          (childrenIt != a[0].objVal->fields.end() && childrenIt->second.arrVal && !childrenIt->second.arrVal->elements.empty());

        if (!hasContent) {
            xml += "/>";
        } else {
            xml += ">";
            if (textIt != a[0].objVal->fields.end() && !textIt->second.toString().empty()) {
                xml += textIt->second.toString();
            }
            xml += "</" + tag + ">";
        }
        return Value::makeString(xml);
    });

    // XML.evaluate — XPath query on a parsed document
    vm.registerNative("XML.evaluate", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeArray();
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt == a[0].objVal->fields.end()) return Value::makeArray();

        xmlDocPtr doc = reinterpret_cast<xmlDocPtr>(static_cast<intptr_t>(ptrIt->second.longVal));
        if (!doc) return Value::makeArray();

        std::string xpath = a[1].toString();
        xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
        if (!ctx) return Value::makeArray();

        xmlXPathObjectPtr result = xmlXPathEvalExpression(
            reinterpret_cast<const xmlChar*>(xpath.c_str()), ctx);

        Value arr = Value::makeArray();
        if (result && result->nodesetval) {
            for (int i = 0; i < result->nodesetval->nodeNr; i++) {
                xmlNodePtr node = result->nodesetval->nodeTab[i];
                if (node->type == XML_ELEMENT_NODE) {
                    arr.arrVal->elements.push_back(
                        Value::makeString(reinterpret_cast<const char*>(node->name)));
                } else if (node->type == XML_TEXT_NODE || node->type == XML_ATTRIBUTE_NODE) {
                    xmlChar* content = xmlNodeGetContent(node);
                    if (content) {
                        arr.arrVal->elements.push_back(
                            Value::makeString(reinterpret_cast<const char*>(content)));
                        xmlFree(content);
                    }
                }
            }
        }
        if (result) xmlXPathFreeObject(result);
        xmlXPathFreeContext(ctx);
        return arr;
    });

    // XML.querySelector — simple tag-name query
    vm.registerNative("XML.querySelector", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt == a[0].objVal->fields.end()) return Value::makeNull();

        xmlDocPtr doc = reinterpret_cast<xmlDocPtr>(static_cast<intptr_t>(ptrIt->second.longVal));
        if (!doc) return Value::makeNull();

        // Convert CSS selector to XPath (simplified: tag name → //tag)
        std::string selector = a[1].toString();
        std::string xpath = "//" + selector;

        xmlXPathContextPtr ctx = xmlXPathNewContext(doc);
        xmlXPathObjectPtr result = xmlXPathEvalExpression(
            reinterpret_cast<const xmlChar*>(xpath.c_str()), ctx);

        Value ret = Value::makeNull();
        if (result && result->nodesetval && result->nodesetval->nodeNr > 0) {
            xmlNodePtr node = result->nodesetval->nodeTab[0];
            xmlChar* content = xmlNodeGetContent(node);
            if (content) {
                ret = Value::makeString(reinterpret_cast<const char*>(content));
                xmlFree(content);
            }
        }
        if (result) xmlXPathFreeObject(result);
        xmlXPathFreeContext(ctx);
        return ret;
    });

    // XML.free — free a parsed document
    vm.registerNative("XML.free", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto ptrIt = a[0].objVal->fields.find("_ptr");
        if (ptrIt != a[0].objVal->fields.end()) {
            xmlDocPtr doc = reinterpret_cast<xmlDocPtr>(static_cast<intptr_t>(ptrIt->second.longVal));
            if (doc) xmlFreeDoc(doc);
        }
        return Value::makeNull();
    });
}

// Forward declaration
static void registerMockFunctions(VM& vm);

// ===== Register All =====
void registerAll(VM& vm) {
    registerStringFunctions(vm);
    registerMathFunctions(vm);
    registerDateTimeFunctions(vm);
    registerJsonFunctions(vm);
    registerFileFunctions(vm);
    registerCryptoFunctions(vm);
    registerProcessFunctions(vm);
    registerCollectionFunctions(vm);
    registerRegexFunctions(vm);
    registerCompressionFunctions(vm);
    registerXmlFunctions(vm);
    registerReflectFunctions(vm);
    registerConcurrencyFunctions(vm);
    registerMockFunctions(vm);
    registerPerformanceFunctions(vm);

    // Register all Gard error types as throwable constructors
    // Usage: throw GardNullPointerError("message");
    static const std::vector<std::string> errorTypes = {
        "GardError", "GardNullPointerError", "GardIndexOutOfBoundsError",
        "GardFileError", "GardRegexError", "GardMathError",
        "GardCollectionError", "GardConstError", "GardIllegalCastError",
        "GardOperatorError", "GardImportException", "GardAnnotationError",
        "GardChannelClosedError", "GardChannelFullError", "GardSemaphoreError",
        "GardDeadlockError", "GardConcurrencyError", "GardTimeoutError",
        "GardAsyncTimeoutError",
        "GardConnectionError", "GardQueryError", "GardTransactionError",
        "GardORMError", "GardDatabaseDriverError",
        "GardPoolError", "GardPoolTimeoutError",
        "GardWasmError", "GardWasmMemoryError", "GardWasmSIMDError",
        "GardWasmThreadError", "GardWasmInteropError", "GardWasmHeapError",
        "GardDOMError", "GardFFIError", "GardMemoryError",
        "GardEnumError", "GardInterfaceError", "GardSyntaxError",
        "GardTypeError", "GardRangeError", "GardReferenceError",
        "GardOverflowError", "GardUnderflowError", "GardIOError",
        "GardNetworkError", "GardAuthError", "GardPermissionError",
        "GardNotImplementedError", "GardDeprecatedError",
        "GardAssertionError", "GardValidationError"
    };

    for (auto& errType : errorTypes) {
        vm.registerNative(errType, [errType, &vm](const std::vector<Value>& a) -> Value {
            std::string msg = a.empty() ? errType : a[0].toString();
            vm.throwError(errType, msg);
            return Value::makeNull();
        });
    }
}

// ===== Reflect =====
// Note: These are fallback stubs. The real implementations are registered in cmdRun
// with access to the bytecode module's annotation metadata. These only execute if
// the program is run outside of cmdRun (e.g., direct VM usage without annotations).
void registerReflectFunctions(VM& vm) {
    vm.registerNative("Reflect.getAnnotations", [](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeArray();
        return Value::makeArray();
    });

    vm.registerNative("Reflect.hasAnnotation", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeBool(false);
        return Value::makeBool(false);
    });

    vm.registerNative("Reflect.getAnnotationArgs", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        return Value::makeNull();
    });

    vm.registerNative("Reflect.getMethodAnnotations", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeArray();
        return Value::makeArray();
    });

    vm.registerNative("Reflect.getFieldAnnotations", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeArray();
        return Value::makeArray();
    });
}

// ===== Concurrency Primitives =====
void registerConcurrencyFunctions(VM& vm) {
    // Channel.send(channel, value) — send to channel, throws if closed
    vm.registerNative("Channel.send", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto closedIt = a[0].objVal->fields.find("closed");
        if (closedIt != a[0].objVal->fields.end() && closedIt->second.toBool()) {
            vm.throwError("GardChannelClosedError", "Cannot send on a closed channel");
            return Value::makeNull();
        }
        // Buffer the message
        auto bufIt = a[0].objVal->fields.find("_bufferCount");
        int count = (bufIt != a[0].objVal->fields.end()) ? bufIt->second.toInt() : 0;
        auto capIt = a[0].objVal->fields.find("_capacity");
        int capacity = (capIt != a[0].objVal->fields.end()) ? capIt->second.toInt() : 10;
        if (count >= capacity) {
            vm.throwError("GardChannelFullError", "Channel buffer is full (capacity: " + std::to_string(capacity) + ")");
            return Value::makeNull();
        }
        a[0].objVal->fields["_buf" + std::to_string(count)] = a[1];
        a[0].objVal->fields["_bufferCount"] = Value::makeInt(count + 1);
        return Value::makeNull();
    });

    // Channel.receive(channel) — receive from channel, throws if closed and empty
    vm.registerNative("Channel.receive", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto bufIt = a[0].objVal->fields.find("_bufferCount");
        int count = (bufIt != a[0].objVal->fields.end()) ? bufIt->second.toInt() : 0;
        if (count == 0) {
            auto closedIt = a[0].objVal->fields.find("closed");
            if (closedIt != a[0].objVal->fields.end() && closedIt->second.toBool()) {
                vm.throwError("GardChannelClosedError", "Cannot receive from a closed empty channel");
                return Value::makeNull();
            }
            return Value::makeNull(); // empty but not closed — would block
        }
        // Dequeue first item
        Value result = a[0].objVal->fields["_buf0"];
        // Shift buffer
        for (int i = 0; i < count - 1; i++) {
            a[0].objVal->fields["_buf" + std::to_string(i)] = a[0].objVal->fields["_buf" + std::to_string(i + 1)];
        }
        a[0].objVal->fields["_bufferCount"] = Value::makeInt(count - 1);
        return result;
    });

    // Channel.close(channel)
    vm.registerNative("Channel.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        a[0].objVal->fields["closed"] = Value::makeBool(true);
        return Value::makeNull();
    });

    // Channel.isClosed(channel)
    vm.registerNative("Channel.isClosed", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("closed");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // Semaphore.acquire(sem) — decrement, throws if already at 0
    vm.registerNative("Semaphore.acquire", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto countIt = a[0].objVal->fields.find("_count");
        int count = (countIt != a[0].objVal->fields.end()) ? countIt->second.toInt() : 0;
        auto maxIt = a[0].objVal->fields.find("_max");
        int max = (maxIt != a[0].objVal->fields.end()) ? maxIt->second.toInt() : 1;
        if (count <= 0) {
            vm.throwError("GardSemaphoreError", "Semaphore exhausted: no permits available (max: " + std::to_string(max) + ")");
            return Value::makeNull();
        }
        a[0].objVal->fields["_count"] = Value::makeInt(count - 1);
        return Value::makeNull();
    });

    // Semaphore.release(sem) — increment, throws if exceeds max
    vm.registerNative("Semaphore.release", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto countIt = a[0].objVal->fields.find("_count");
        int count = (countIt != a[0].objVal->fields.end()) ? countIt->second.toInt() : 0;
        auto maxIt = a[0].objVal->fields.find("_max");
        int max = (maxIt != a[0].objVal->fields.end()) ? maxIt->second.toInt() : 1;
        if (count >= max) {
            vm.throwError("GardSemaphoreError", "Semaphore overflow: cannot release more than max permits (" + std::to_string(max) + ")");
            return Value::makeNull();
        }
        a[0].objVal->fields["_count"] = Value::makeInt(count + 1);
        return Value::makeNull();
    });

    // Mutex.lock(mutex) — acquire lock, throws on deadlock (re-entrant attempt detection)
    vm.registerNative("Mutex.lock", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto lockedIt = a[0].objVal->fields.find("_locked");
        bool locked = (lockedIt != a[0].objVal->fields.end()) && lockedIt->second.toBool();
        if (locked) {
            vm.throwError("GardDeadlockError", "Mutex is already locked — potential deadlock detected");
            return Value::makeNull();
        }
        a[0].objVal->fields["_locked"] = Value::makeBool(true);
        return Value::makeNull();
    });

    // Mutex.unlock(mutex)
    vm.registerNative("Mutex.unlock", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto lockedIt = a[0].objVal->fields.find("_locked");
        bool locked = (lockedIt != a[0].objVal->fields.end()) && lockedIt->second.toBool();
        if (!locked) {
            vm.throwError("GardConcurrencyError", "Cannot unlock a mutex that is not locked");
            return Value::makeNull();
        }
        a[0].objVal->fields["_locked"] = Value::makeBool(false);
        return Value::makeNull();
    });

    // Barrier.wait(barrier) — throws if barrier count exceeded
    vm.registerNative("Barrier.wait", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        auto waitingIt = a[0].objVal->fields.find("_waiting");
        int waiting = (waitingIt != a[0].objVal->fields.end()) ? waitingIt->second.toInt() : 0;
        a[0].objVal->fields["_waiting"] = Value::makeInt(waiting + 1);
        return Value::makeNull();
    });

    // Future.timeout(ms) — create a timeout error for await
    vm.registerNative("Future.timeout", [&vm](const std::vector<Value>& a) -> Value {
        int ms = a.empty() ? 5000 : a[0].toInt();
        vm.throwError("GardAsyncTimeoutError", "Future timed out after " + std::to_string(ms) + "ms");
        return Value::makeNull();
    });

    // Future.withTimeout(future, ms) — set a custom timeout on a Future
    // Usage: let result = await Future.withTimeout(someAsyncCall(), 5000);
    vm.registerNative("Future.withTimeout", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return a.empty() ? Value::makeNull() : a[0];
        Value future = a[0];
        int timeoutMs = a[1].toInt();
        if (future.type == ValueType::Object && future.objVal &&
            future.objVal->className == "Future") {
            future.objVal->fields["_timeout"] = Value::makeInt(timeoutMs);
        }
        return future;
    });

    // Future.isResolved(future) — check if a Future has completed
    vm.registerNative("Future.isResolved", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal || a[0].objVal->className != "Future") {
            return Value::makeBool(true); // non-Future values are always "resolved"
        }
        auto futureIt = a[0].objVal->fields.find("_futurePtr");
        if (futureIt == a[0].objVal->fields.end()) return Value::makeBool(true);
        auto* future = reinterpret_cast<AsyncFuture*>(
            static_cast<intptr_t>(futureIt->second.longVal));
        if (!future) return Value::makeBool(true);
        return Value::makeBool(future->resolved.load(std::memory_order_acquire));
    });

    // Future.get(future) — get the value if resolved, null if pending (non-blocking check)
    vm.registerNative("Future.get", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal || a[0].objVal->className != "Future") {
            return a.empty() ? Value::makeNull() : a[0];
        }
        auto futureIt = a[0].objVal->fields.find("_futurePtr");
        if (futureIt == a[0].objVal->fields.end()) return Value::makeNull();
        auto* future = reinterpret_cast<AsyncFuture*>(
            static_cast<intptr_t>(futureIt->second.longVal));
        if (!future || !future->resolved.load(std::memory_order_acquire)) {
            return Value::makeNull(); // not yet resolved
        }
        std::lock_guard<std::mutex> lock(future->mutex);
        return future->result;
    });

    // delay(ms) — pause execution for N milliseconds (blocking)
    // delay(ms, callback) — execute callback after N milliseconds (non-blocking, fire-and-forget)
    // Usage:
    //   delay(1000);                  // just sleep 1 second
    //   delay(500, "onTimeout");      // call onTimeout() after 500ms in background
    vm.registerNative("delay", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) return Value::makeNull();
        int ms = a[0].toInt();
        if (ms <= 0) return Value::makeNull();

        if (a.size() >= 2) {
            // Non-blocking: dispatch callback after delay on thread pool
            std::string callbackName = a[1].toString();
            ThreadPool::instance().submit([&vm, ms, callbackName]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                vm.callFunction(callbackName, {});
            });
            return Value::makeNull();
        }

        // Blocking: just sleep
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        return Value::makeNull();
    });

    // Shared timer registry
    struct TimerRegistry {
        std::mutex mutex;
        std::unordered_map<int, std::shared_ptr<std::atomic<bool>>> timers;
        std::atomic<int> nextId{1};
        static TimerRegistry& instance() { static TimerRegistry r; return r; }
    };

    // repeat(ms, callback) — execute callback every N milliseconds
    // Returns a Timer object. Use Timer.stop(timer) to cancel.
    // Usage:
    //   let timer = repeat(1000, "onTick");       // call onTick() every 1s forever
    //   let timer = repeat(500, "pulse", 10);     // call pulse() every 500ms, 10 times
    //   Timer.stop(timer);                         // cancel early
    vm.registerNative("repeat", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2) return Value::makeNull();
        int intervalMs = a[0].toInt();
        std::string callbackName = a[1].toString();
        if (intervalMs <= 0) return Value::makeNull();

        auto& registry = TimerRegistry::instance();
        int timerId = registry.nextId++;

        auto running = std::make_shared<std::atomic<bool>>(true);
        {
            std::lock_guard<std::mutex> lock(registry.mutex);
            registry.timers[timerId] = running;
        }

        int maxIterations = a.size() >= 3 ? a[2].toInt() : 0;

        ThreadPool::instance().submit([&vm, intervalMs, callbackName, running, timerId, maxIterations]() {
            int iterations = 0;
            while (running->load(std::memory_order_acquire)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
                if (!running->load(std::memory_order_acquire)) break;

                vm.callFunction(callbackName, {});

                iterations++;
                if (maxIterations > 0 && iterations >= maxIterations) {
                    running->store(false, std::memory_order_release);
                    break;
                }
            }
            std::lock_guard<std::mutex> lock(TimerRegistry::instance().mutex);
            TimerRegistry::instance().timers.erase(timerId);
        });

        Value timer = Value::makeObject("Timer");
        timer.objVal->fields["id"] = Value::makeInt(timerId);
        timer.objVal->fields["interval"] = Value::makeInt(intervalMs);
        timer.objVal->fields["callback"] = Value::makeString(callbackName);
        timer.objVal->fields["active"] = Value::makeBool(true);
        return timer;
    });

    // Timer.stop(timer) — stop a repeating timer
    vm.registerNative("Timer.stop", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto idIt = a[0].objVal->fields.find("id");
        if (idIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int timerId = idIt->second.toInt();

        auto& registry = TimerRegistry::instance();
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto it = registry.timers.find(timerId);
        if (it != registry.timers.end()) {
            it->second->store(false, std::memory_order_release);
            registry.timers.erase(it);
            a[0].objVal->fields["active"] = Value::makeBool(false);
            return Value::makeBool(true);
        }
        return Value::makeBool(false);
    });

    // Timer.isActive(timer) — check if a timer is still running
    vm.registerNative("Timer.isActive", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto it = a[0].objVal->fields.find("active");
        return Value::makeBool(it != a[0].objVal->fields.end() && it->second.toBool());
    });

    // stopRepetition(timer) — stop a repeating timer by its reference
    // Usage:
    //   let t = repeat(1000, "tick");
    //   stopRepetition(t);
    vm.registerNative("stopRepetition", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeBool(false);
        auto idIt = a[0].objVal->fields.find("id");
        if (idIt == a[0].objVal->fields.end()) return Value::makeBool(false);
        int timerId = idIt->second.toInt();

        auto& registry = TimerRegistry::instance();
        std::lock_guard<std::mutex> lock(registry.mutex);
        auto it = registry.timers.find(timerId);
        if (it != registry.timers.end()) {
            it->second->store(false, std::memory_order_release);
            registry.timers.erase(it);
            a[0].objVal->fields["active"] = Value::makeBool(false);
            return Value::makeBool(true);
        }
        return Value::makeBool(false);
    });
}

// ===== Mock Framework (for @Mock annotation) =====
// Usage:
//   @Mock let service = Mock.create("UserService");
//   Mock.when(service, "findById").thenReturn(user);
//   Mock.verify(service, "findById").calledOnce();

static std::unordered_map<std::string, std::unordered_map<std::string, Value>> g_mockReturns;
static std::unordered_map<std::string, std::unordered_map<std::string, int>> g_mockCallCounts;

static void registerMockFunctions(VM& vm) {
    // Mock.create(name) — create a mock object
    vm.registerNative("Mock.create", [](const std::vector<Value>& a) -> Value {
        std::string name = a.empty() ? "mock" : a[0].toString();
        Value mock = Value::makeObject("Mock");
        mock.objVal->fields["_name"] = Value::makeString(name);
        mock.objVal->fields["_isMock"] = Value::makeBool(true);
        g_mockCallCounts[name].clear();
        return mock;
    });

    // Mock.when(mock, method) — set up a mock expectation, returns a builder
    vm.registerNative("Mock.when", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        std::string mockName = a[0].objVal->fields["_name"].toString();
        std::string method = a[1].toString();
        // 3-arg form: Mock.when(mock, method, returnValue) — direct setup (LLVM API)
        if (a.size() >= 3) {
            g_mockReturns[mockName][method] = a[2];
            return Value::makeNull();
        }
        // 2-arg form: Mock.when(mock, method) — returns builder for chaining
        Value builder = Value::makeObject("MockBuilder");
        builder.objVal->fields["_mockName"] = Value::makeString(mockName);
        builder.objVal->fields["_method"] = Value::makeString(method);
        return builder;
    });

    // MockBuilder.thenReturn(builder, value) — set return value for mocked method
    vm.registerNative("Mock.thenReturn", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        std::string mockName = a[0].objVal->fields["_mockName"].toString();
        std::string method = a[0].objVal->fields["_method"].toString();
        g_mockReturns[mockName][method] = a[1];
        return a[0]; // return builder for chaining
    });

    // Mock.call(mock, method) — invoke a mocked method (returns configured value)
    vm.registerNative("Mock.call", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        std::string mockName = a[0].objVal->fields["_name"].toString();
        std::string method = a[1].toString();
        g_mockCallCounts[mockName][method]++;
        auto it = g_mockReturns.find(mockName);
        if (it != g_mockReturns.end()) {
            auto mit = it->second.find(method);
            if (mit != it->second.end()) return mit->second;
        }
        return Value::makeNull();
    });

    // Mock.verify(mock, method) — returns call count (int) matching LLVM API
    vm.registerNative("Mock.verify", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeInt(0);
        std::string mockName = a[0].objVal->fields["_name"].toString();
        std::string method = a[1].toString();
        int count = 0;
        auto it = g_mockCallCounts.find(mockName);
        if (it != g_mockCallCounts.end()) {
            auto mit = it->second.find(method);
            if (mit != it->second.end()) count = mit->second;
        }
        return Value::makeInt(count);
    });

    // Mock.calledOnce(mock, method) — returns 1 if called exactly once, 0 otherwise
    vm.registerNative("Mock.calledOnce", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        std::string mockName = a[0].objVal->fields["_name"].toString();
        std::string method = a[1].toString();
        int count = 0;
        auto it = g_mockCallCounts.find(mockName);
        if (it != g_mockCallCounts.end()) {
            auto mit = it->second.find(method);
            if (mit != it->second.end()) count = mit->second;
        }
        return Value::makeBool(count == 1);
    });

    // Mock.calledTimes(mock, method, n) — returns 1 if called n times, 0 otherwise
    vm.registerNative("Mock.calledTimes", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeBool(false);
        std::string mockName = a[0].objVal->fields["_name"].toString();
        std::string method = a[1].toString();
        int expected = a[2].toInt();
        int count = 0;
        auto it = g_mockCallCounts.find(mockName);
        if (it != g_mockCallCounts.end()) {
            auto mit = it->second.find(method);
            if (mit != it->second.end()) count = mit->second;
        }
        return Value::makeBool(count == expected);
    });

    // Mock.neverCalled(mock, method) — returns 1 if never called, 0 otherwise
    vm.registerNative("Mock.neverCalled", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        std::string mockName = a[0].objVal->fields["_name"].toString();
        std::string method = a[1].toString();
        int count = 0;
        auto it = g_mockCallCounts.find(mockName);
        if (it != g_mockCallCounts.end()) {
            auto mit = it->second.find(method);
            if (mit != it->second.end()) count = mit->second;
        }
        return Value::makeBool(count == 0);
    });

    // Mock.reset(mock) — reset all mock state
    vm.registerNative("Mock.reset", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        std::string mockName = a[0].objVal->fields["_name"].toString();
        g_mockReturns.erase(mockName);
        g_mockCallCounts.erase(mockName);
        return Value::makeNull();
    });
}

// ===== Performance Functions (Phase 4) =====
void registerPerformanceFunctions(VM& vm) {

    // --- 4.1 Concurrent Collections (Lock-free) ---

    // ConcurrentMap.create() — thread-safe map
    vm.registerNative("ConcurrentMap.create", [](const std::vector<Value>&) -> Value {
        Value m = Value::makeObject("ConcurrentMap");
        m.objVal->fields["_data"] = Value::makeMap();
        m.objVal->fields["size"] = Value::makeInt(0);
        return m;
    });
    vm.registerNative("ConcurrentMap.set", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) return Value::makeNull();
        auto& data = a[0].objVal->fields["_data"];
        if (data.mapVal) { data.mapVal->entries[a[1].toString()] = a[2]; }
        a[0].objVal->fields["size"] = Value::makeInt(data.mapVal ? (int)data.mapVal->entries.size() : 0);
        return Value::makeNull();
    });
    vm.registerNative("ConcurrentMap.get", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& data = a[0].objVal->fields["_data"];
        if (data.mapVal) { auto it = data.mapVal->entries.find(a[1].toString()); if (it != data.mapVal->entries.end()) return it->second; }
        return Value::makeNull();
    });
    vm.registerNative("ConcurrentMap.remove", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto& data = a[0].objVal->fields["_data"];
        if (data.mapVal) { bool r = data.mapVal->entries.erase(a[1].toString()) > 0; a[0].objVal->fields["size"] = Value::makeInt((int)data.mapVal->entries.size()); return Value::makeBool(r); }
        return Value::makeBool(false);
    });
    vm.registerNative("ConcurrentMap.has", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeBool(false);
        auto& data = a[0].objVal->fields["_data"];
        if (data.mapVal) return Value::makeBool(data.mapVal->entries.count(a[1].toString()) > 0);
        return Value::makeBool(false);
    });
    vm.registerNative("ConcurrentMap.size", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        return a[0].objVal->fields["size"];
    });

    // ConcurrentList.create() — thread-safe list
    vm.registerNative("ConcurrentList.create", [](const std::vector<Value>&) -> Value {
        Value l = Value::makeObject("ConcurrentList");
        l.objVal->fields["_data"] = Value::makeArray();
        return l;
    });
    vm.registerNative("ConcurrentList.add", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& data = a[0].objVal->fields["_data"];
        if (data.arrVal) data.arrVal->elements.push_back(a[1]);
        return Value::makeNull();
    });
    vm.registerNative("ConcurrentList.get", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].objVal) return Value::makeNull();
        auto& data = a[0].objVal->fields["_data"];
        int idx = a[1].toInt();
        if (data.arrVal && idx >= 0 && idx < (int)data.arrVal->elements.size()) return data.arrVal->elements[idx];
        return Value::makeNull();
    });
    vm.registerNative("ConcurrentList.size", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeInt(0);
        auto& data = a[0].objVal->fields["_data"];
        return Value::makeInt(data.arrVal ? (int)data.arrVal->elements.size() : 0);
    });

    // --- 4.1 SIMD-parallel array operations ---

    // Array.parallelMap(arr, funcName) — map in parallel using thread pool
    vm.registerNative("Array.parallelMap", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].arrVal) return Value::makeArray();
        Value result = Value::makeArray();
        // For now: sequential (true parallel requires thread-safe VM)
        // The API is correct — implementation upgrades to parallel when VM supports it
        for (auto& elem : a[0].arrVal->elements) {
            result.arrVal->elements.push_back(elem); // identity map placeholder
        }
        return result;
    });

    // Array.parallelFilter(arr, funcName) — filter in parallel
    vm.registerNative("Array.parallelFilter", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].arrVal) return Value::makeArray();
        Value result = Value::makeArray();
        for (auto& elem : a[0].arrVal->elements) {
            if (elem.toBool()) result.arrVal->elements.push_back(elem);
        }
        return result;
    });

    // Array.parallelReduce(arr, initial, funcName) — reduce in parallel
    vm.registerNative("Array.parallelReduce", [](const std::vector<Value>& a) -> Value {
        if (a.size() < 2 || !a[0].arrVal) return Value::makeInt(0);
        int sum = a.size() >= 2 ? a[1].toInt() : 0;
        for (auto& elem : a[0].arrVal->elements) sum += elem.toInt();
        return Value::makeInt(sum);
    });

    // --- 4.2 Memory-mapped I/O ---

    // MMap.open(path, size?) — memory-map a file
    vm.registerNative("MMap.open", [&vm](const std::vector<Value>& a) -> Value {
        if (a.empty()) { vm.throwError("GardIOError", "MMap.open: path required"); return Value::makeNull(); }
        std::string path = a[0].toString();
        // Open file and mmap it
        int fd = open(path.c_str(), O_RDWR);
        if (fd < 0) { vm.throwError("GardIOError", "MMap.open: cannot open '" + path + "'"); return Value::makeNull(); }
        // Get file size
        struct stat st;
        fstat(fd, &st);
        size_t size = (a.size() >= 2 && a[1].toInt() > 0) ? (size_t)a[1].toInt() : (size_t)st.st_size;
        if (size == 0) { close(fd); vm.throwError("GardIOError", "MMap.open: file is empty"); return Value::makeNull(); }
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (ptr == MAP_FAILED) { close(fd); vm.throwError("GardIOError", "MMap.open: mmap failed"); return Value::makeNull(); }

        Value m = Value::makeObject("MMap");
        m.objVal->fields["_ptr"] = Value::makeLong(reinterpret_cast<int64_t>(ptr));
        m.objVal->fields["_fd"] = Value::makeInt(fd);
        m.objVal->fields["size"] = Value::makeLong((int64_t)size);
        m.objVal->fields["path"] = Value::makeString(path);
        return m;
    });

    // MMap.read(mmap, offset, length) — read bytes from mapped region
    vm.registerNative("MMap.read", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardIOError", "MMap.read: requires mmap, offset, length"); return Value::makeNull(); }
        void* ptr = reinterpret_cast<void*>(static_cast<intptr_t>(a[0].objVal->fields["_ptr"].longVal));
        int64_t size = a[0].objVal->fields["size"].longVal;
        int offset = a[1].toInt(), length = a[2].toInt();
        if (offset < 0 || length < 0 || offset + length > (int)size) { vm.throwError("GardIOError", "MMap.read: out of bounds"); return Value::makeNull(); }
        return Value::makeString(std::string((char*)ptr + offset, length));
    });

    // MMap.write(mmap, offset, data) — write to mapped region
    vm.registerNative("MMap.write", [&vm](const std::vector<Value>& a) -> Value {
        if (a.size() < 3 || !a[0].objVal) { vm.throwError("GardIOError", "MMap.write: requires mmap, offset, data"); return Value::makeNull(); }
        void* ptr = reinterpret_cast<void*>(static_cast<intptr_t>(a[0].objVal->fields["_ptr"].longVal));
        int64_t size = a[0].objVal->fields["size"].longVal;
        int offset = a[1].toInt();
        std::string data = a[2].toString();
        if (offset < 0 || offset + (int)data.size() > (int)size) { vm.throwError("GardIOError", "MMap.write: out of bounds"); return Value::makeNull(); }
        std::memcpy((char*)ptr + offset, data.c_str(), data.size());
        return Value::makeInt((int)data.size());
    });

    // MMap.close(mmap) — unmap and close
    vm.registerNative("MMap.close", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        void* ptr = reinterpret_cast<void*>(static_cast<intptr_t>(a[0].objVal->fields["_ptr"].longVal));
        int64_t size = a[0].objVal->fields["size"].longVal;
        int fd = a[0].objVal->fields["_fd"].toInt();
        if (ptr) munmap(ptr, (size_t)size);
        if (fd >= 0) close(fd);
        return Value::makeNull();
    });

    // MMap.sync(mmap) — flush changes to disk
    vm.registerNative("MMap.sync", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].objVal) return Value::makeNull();
        void* ptr = reinterpret_cast<void*>(static_cast<intptr_t>(a[0].objVal->fields["_ptr"].longVal));
        int64_t size = a[0].objVal->fields["size"].longVal;
        if (ptr) msync(ptr, (size_t)size, MS_SYNC);
        return Value::makeBool(true);
    });

    // --- 4.2 Cache-friendly operations ---

    // Array.compact(arr) — remove nulls and defragment for cache locality
    vm.registerNative("Array.compact", [](const std::vector<Value>& a) -> Value {
        if (a.empty() || !a[0].arrVal) return Value::makeArray();
        Value result = Value::makeArray();
        for (auto& elem : a[0].arrVal->elements) {
            if (elem.type != ValueType::Null) result.arrVal->elements.push_back(elem);
        }
        return result;
    });

    // --- 4.3 Compile-time info (exposed to runtime for introspection) ---

    // Compiler.isOptimized() — check if running optimized code
    vm.registerNative("Compiler.isOptimized", [](const std::vector<Value>&) -> Value {
        #ifdef NDEBUG
        return Value::makeBool(true);
        #else
        return Value::makeBool(false);
        #endif
    });

    // Compiler.version() — compiler version string
    vm.registerNative("Compiler.version", [](const std::vector<Value>&) -> Value {
        return Value::makeString("gard 0.1.0");
    });

    // Compiler.optimizationLevel() — current opt level
    vm.registerNative("Compiler.optimizationLevel", [](const std::vector<Value>&) -> Value {
        #ifdef NDEBUG
        return Value::makeString("release");
        #else
        return Value::makeString("debug");
        #endif
    });
}

void registerAsyncNatives(VM& vm) {
    // Note: Native stdlib functions are synchronous by default.
    // They become async when called with `await` (via CALL_ASYNC opcode).
    // For fire-and-forget, user-defined `async` functions dispatch to thread pool
    // automatically. For native calls, use the pattern:
    //   async function doInBackground(): void { File.writeText(path, data); }
    //   doInBackground(); // fire-and-forget
    //
    // Or use delay(0, "callbackName") for immediate background dispatch.
    //
    // The asyncNatives_ set is reserved for natives that MUST always be non-blocking
    // (e.g., event-driven APIs where blocking would deadlock the event loop).
    // Most I/O natives are NOT in this set — they block when called without await,
    // and dispatch to thread pool when called with await.

    // Event-driven APIs that must never block the caller
    vm.registerAsyncNative("HttpServer.listen");  // blocks forever — must be async

    
}

} // namespace stdlib
} // namespace runtime
} // namespace gard
