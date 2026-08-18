// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// Host shim: LittleFS over a real directory tree, rooted at $STORE_TEST_ROOT
// (default ./fsroot). Enough of File/FS for src/store.cpp.
#pragma once
#include "Arduino.h"
#include <cstdio>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_READ "r"

std::string fsPath(const char* p);

class File {
public:
    File() {}
    File(FILE* f, long sz) : f_(f), size_(sz) {}
    explicit operator bool() const { return f_ != nullptr; }
    void close() { if (f_) { fclose(f_); f_ = nullptr; } }

    size_t read(uint8_t* buf, size_t n) { return f_ ? fread(buf, 1, n, f_) : 0; }
    int    read()                       { return f_ ? fgetc(f_) : -1; }
    bool   available()                  { if (!f_) return false; int c = fgetc(f_);
                                          if (c == EOF) return false; ungetc(c, f_); return true; }
    // Arduino's print() returns the byte count actually written. store.cpp
    // depends on that: a short return is how a full filesystem is detected.
    size_t print(const String& s) { return f_ ? fwrite(s.c_str(), 1, s.length(), f_) : 0; }
    size_t print(const char* s)   { return f_ ? fwrite(s, 1, strlen(s), f_) : 0; }
    size_t print(char c)          { return f_ ? fwrite(&c, 1, 1, f_) : 0; }
    size_t write(const uint8_t* b, size_t n) { return f_ ? fwrite(b, 1, n, f_) : 0; }
    void   flush()                { if (f_) fflush(f_); }
    long   size() const           { return size_; }
    bool   seek(long p)           { return f_ && fseek(f_, p, SEEK_SET) == 0; }
private:
    FILE* f_ = nullptr;
    long  size_ = 0;
};

class LittleFSShim {
public:
    bool begin(bool format = false);
    bool exists(const char* p);
    File open(const char* p, const char* mode = FILE_READ);
    bool remove(const char* p);
    bool rename(const char* a, const char* b);
    bool mkdir(const char* p);
    size_t totalBytes() { return 2u * 1024 * 1024; }   // matches the 2 MB partition
    size_t usedBytes();
};
extern LittleFSShim LittleFS;
