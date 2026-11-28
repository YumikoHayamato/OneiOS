/* The part of AOSP's <cutils/properties.h> that libvirglrenderer uses (src/mesa/util/os_misc.c); property_get is
 * cutils_shim.c's (over bionic's __system_property_get, as libcutils' own). */
#pragma once
#include <sys/system_properties.h>
#define PROPERTY_KEY_MAX PROP_NAME_MAX
#define PROPERTY_VALUE_MAX PROP_VALUE_MAX
int property_get(const char *key, char *value, const char *default_value);
