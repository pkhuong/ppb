#pragma once

#define likely(X)   __builtin_expect(!!(X), 1)
#define unlikely(X) __builtin_expect(!!(X), 0)

#define NOINLINE    __attribute__((__noinline__))
