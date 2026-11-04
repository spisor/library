#ifndef RUNTIME_FETCH_HOOKS_H
#define RUNTIME_FETCH_HOOKS_H

/**
 * Lets fetch.get take plain http: urls, which only a loopback test fixture serves, since the fixture
 * carries no TLS of its own.
 *
 * @implNote Defined only when SPISOR_FETCH_TEST_HOOKS is set, which only the test's own build of the
 * fetch sources does; the library carries no such symbol and refuses http: always.
 */
void runtime_fetch_allow_plain_http(int allowed);

/** Replaces the 75 second deadline of one fetch; 0 restores it. */
void runtime_fetch_use_deadline(unsigned milliseconds);

#if defined(_WIN32)
/** The WinHTTP access type every session is opened with. */
unsigned long runtime_fetch_windows_access_type(void);
#else
/**
 * Forgets any loaded libcurl and makes the next fetch open the shared object called library_name instead
 * of the platform's own; NULL restores the default.
 */
void runtime_fetch_curl_use_library(const char *library_name);
#endif

#endif
