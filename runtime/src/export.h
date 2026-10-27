#ifndef RUNTIME_EXPORT_H
#define RUNTIME_EXPORT_H

#if defined(_WIN32)
#define RUNTIME_EXPORT __declspec(dllexport)
#else
#define RUNTIME_EXPORT __attribute__((visibility("default")))
#endif

#endif
