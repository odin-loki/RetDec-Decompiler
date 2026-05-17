/**
 * @file src/fileformat/types/certificate_table/certificate_table.cpp
 * @brief Class for certificate table.
 * @copyright (c) 2021 Odin Loch Trading as Imortek
 */

#include "retdec/fileformat/types/certificate_table/certificate_table.h"

namespace retdec {
namespace fileformat {

CertificateTable::CertificateTable(std::vector<DigitalSignature> signatures)
	: signatures(signatures) {}
/**
 * Check if certificate table is empty
 * @return @c true if table does not contain any certificates, @c false otherwise
 */
bool CertificateTable::empty() const
{
	return signatures.empty();
}

} // namespace fileformat
} // namespace retdec
