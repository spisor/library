#ifndef RUNTIME_INSTALL_HOOKS_H
#define RUNTIME_INSTALL_HOOKS_H

#include "install_methods.h"

/**
 * The fake install method, which records the packages it installed in cache/methods/fake/installed.json:
 * install records 1.0.0, update moves an installed package to 2.0.0 and refuses one that is not
 * installed, remove forgets it.
 *
 * @implNote Defined only when SPISOR_INSTALL_TEST_HOOKS is set, which only the test's own build of the
 * install sources does; the library carries no method a test could reach (plan decision 5).
 */
extern const runtime_install_method runtime_install_fake_method;

/**
 * Makes install.runtime acquire version from url, whose archive has sha256, in place of the pinned
 * build for this platform, the archive's kind read from the url's .zip or .tar.gz ending; a NULL url
 * restores the pin.
 */
void runtime_install_use_runtime_build(const char *version, const char *url, const char *sha256);

/**
 * Calls hook with the staging folder of each runtime acquisition once it is built and before it is moved
 * into place, so a test can hold a file in it open as a virus scanner would; NULL stops it.
 */
void runtime_install_before_publish(void (*hook)(const char *staging));

#endif
