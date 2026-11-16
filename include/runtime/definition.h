#ifndef RUNTIME_PUBLIC_DEFINITION_H
#define RUNTIME_PUBLIC_DEFINITION_H

/**
 * @file
 * How runtime declares the symbol of each service's definition, a `const void *f(void)` answering that service's
 * static struct.
 *
 * @implNote Exported only while runtime itself is built (RUNTIME_BUILDING), since MSVC refuses a definition whose
 * linkage differs from its declaration, and a consumer declaring an import it links statically would fail instead.
 */

#if defined(_WIN32) && defined(RUNTIME_BUILDING)
#define RUNTIME_DEFINITION_API __declspec(dllexport)
#elif defined(_WIN32)
#define RUNTIME_DEFINITION_API
#else
#define RUNTIME_DEFINITION_API __attribute__((visibility("default")))
#endif

#endif
