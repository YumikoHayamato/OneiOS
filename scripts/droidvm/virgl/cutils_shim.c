/* libcutils' property_get, for an NDK build of libvirglrenderer (no libcutils in the NDK). The same semantics as
 * system/core/libcutils/properties.cpp: the property's value, or default_value when it is empty or unset; returns the
 * length written (without the NUL). Hidden: it serves only this library's src/mesa/util/os_misc.c. */
#include <string.h>
#include <sys/system_properties.h>

#define PROPERTY_VALUE_MAX PROP_VALUE_MAX

__attribute__((visibility("hidden"))) int property_get(const char *key, char *value, const char *default_value)
{
   int len = __system_property_get(key, value);
   if (len > 0)
      return len;
   if (default_value) {
      len = (int)strnlen(default_value, PROPERTY_VALUE_MAX - 1);
      memcpy(value, default_value, (size_t)len);
      value[len] = '\0';
      return len;
   }
   value[0] = '\0';
   return 0;
}
