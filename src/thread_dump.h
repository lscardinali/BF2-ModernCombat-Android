#pragma once

// Writes every thread's stack to `path`, `repeat` times, every `seconds` seconds, from a thread of its own.
void ThreadDumpAfter(const char* path, int seconds, int repeat);
