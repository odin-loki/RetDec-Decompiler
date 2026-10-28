#ifndef AUTHENTICODE_PARSER_ASN1_COMPAT_H
#define AUTHENTICODE_PARSER_ASN1_COMPAT_H

#include <openssl/asn1.h>

/* OpenSSL 3.4 and 4 keep asn1_string_st opaque in the public headers.
 * ASN1_STRING_length / ASN1_STRING_get0_data have been the accessors since 1.1.0. */
static inline int ac_asn1_len(const ASN1_STRING* s)
{
	return ASN1_STRING_length(s);
}

static inline const unsigned char* ac_asn1_data(const ASN1_STRING* s)
{
	return ASN1_STRING_get0_data(s);
}

#endif
