// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl
//
// Page: an HTML page built in small blocks and sent in chunks.
//
// Why (2026-11-18, found live): pages used to be one Arduino String. With the
// real store (94 spools, ~40 Stock List items, ~90 products) Restock is ~83 KB
// and the Stock List page ~96 KB. Growing one String that large needs one
// contiguous block of that size (and a second one while realloc copies), and
// the station's heap is ~190 KB and fragmented. When an append failed, Arduino
// String dropped it SILENTLY and carried on, so later, smaller appends landed
// after the gap: on an iPad the Restock list ran straight into the raw text of
// a script from the end of the page, the list's own script and styles were
// missing, and nothing could be tapped or searched. Sending the finished String
// then copied it again.
//
// So a page is a list of blocks of at most PAGE_BLOCK bytes (no large
// allocation at all), is streamed to the browser block by block with a chunked
// response (no copy; each block is freed once it has gone), and remembers if
// anything failed to land, in which case sendPage() answers 503 with a short
// "reload" page instead of sending a spliced one. A temporary String that
// failed to build (Arduino String invalidates itself: c_str() is null) counts
// as a failed append too.
#pragma once
#include <Arduino.h>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <vector>

// Above CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL (4096), so blocks go to PSRAM
// when it is working and leave internal RAM to WiFi; small enough to fit
// anywhere when it is not.
static constexpr size_t PAGE_BLOCK = 8192;

class Page {
public:
    Page() {}
    Page(const String& s) { *this += s; }
    Page(const char* s)   { *this += s; }
    ~Page() { for (char* b : blocks_) free(b); }
    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    Page& operator+=(const char* s) {
        if (!s) { failed_ = true; return *this; }
        append(s, strlen(s));
        return *this;
    }
    Page& operator+=(const String& s) {
        const char* c = s.c_str();
        if (!c) { failed_ = true; return *this; }   // an invalidated String
        append(c, s.length());
        return *this;
    }
    Page& operator+=(char c) { append(&c, 1); return *this; }

    void swap(Page& o) {
        blocks_.swap(o.blocks_);
        lens_.swap(o.lens_);
        std::swap(total_, o.total_);
        std::swap(head_, o.head_);
        std::swap(off_, o.off_);
        std::swap(failed_, o.failed_);
    }

    size_t length() const { return total_; }
    bool   failed() const { return failed_; }

    // Copies up to maxLen bytes starting at the read position into buf and
    // frees blocks that are fully sent. Returns the number of bytes copied;
    // 0 once everything has gone.
    size_t read(uint8_t* buf, size_t maxLen) {
        size_t n = 0;
        while (n < maxLen && head_ < blocks_.size()) {
            const size_t have = lens_[head_] - off_;
            const size_t take = have < maxLen - n ? have : maxLen - n;
            memcpy(buf + n, blocks_[head_] + off_, take);
            n += take;
            off_ += take;
            if (off_ == lens_[head_]) {
                free(blocks_[head_]);
                blocks_[head_] = nullptr;
                head_++;
                off_ = 0;
            }
        }
        return n;
    }

private:
    void append(const char* s, size_t n) {
        while (n) {
            if (blocks_.empty() || lens_.back() == PAGE_BLOCK) {
                char* b = (char*)malloc(PAGE_BLOCK);
                if (!b) { failed_ = true; return; }
                blocks_.push_back(b);
                lens_.push_back(0);
            }
            const size_t room = PAGE_BLOCK - lens_.back();
            const size_t take = n < room ? n : room;
            memcpy(blocks_.back() + lens_.back(), s, take);
            lens_.back() += take;
            total_ += take;
            s += take;
            n -= take;
        }
    }
    std::vector<char*>  blocks_;
    std::vector<size_t> lens_;
    size_t total_ = 0;
    size_t head_ = 0, off_ = 0;
    bool   failed_ = false;
};
