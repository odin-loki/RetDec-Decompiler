/**
 * @file SecurityDirectory.cpp
 * @brief Class for security directory.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include "retdec/pelib/PeLibInc.h"

#include <cstdint>
#include <utility>
#include "retdec/pelib/SecurityDirectory.h"

namespace PeLib {
SecurityDirectory::SecurityDirectory(): m_ldrError(LDR_ERROR_NONE) {}

unsigned int SecurityDirectory::calcNumberOfCertificates() const
{
	return (unsigned int)m_certs.size();
}

const std::vector<unsigned char>& SecurityDirectory::getCertificate(std::size_t index) const
{
	return m_certs[index].Certificate;
}

LoaderError SecurityDirectory::loaderError() const
{
	return m_ldrError;
}

int SecurityDirectory::read(std::istream& inStream, unsigned int uiOffset, unsigned int uiSize)
{
	IStreamWrapper inStream_w(inStream);

	m_ldrError = LDR_ERROR_NONE;

	if (!inStream_w)
	{
		return ERROR_OPENING_FILE;
	}

	std::uint64_t ulFileSize = fileSize(inStream_w);

	// uiOffset and uiSize are 32-bit and come from a data directory, so
	// `uiOffset + uiSize` wraps: 0xFFFFFF00 + 0x200 is 0x100, which passes
	// a "does it fit" test while naming a range far outside the file. The
	// allocation below then trusts uiSize and asks for up to 4 GB.
	// Comparing in 64 bits, in subtraction form, never forms the sum and
	// bounds uiSize by the file that has to supply it.
	if (uiOffset > ulFileSize || uiSize > ulFileSize - uiOffset)
	{
		m_ldrError = LDR_ERROR_DIGITAL_SIGNATURE_CUT;
		return ERROR_INVALID_FILE;
	}

	inStream_w.seekg(uiOffset, std::ios::beg);

	std::vector<unsigned char> vCertDirectory(uiSize);
	inStream_w.read(reinterpret_cast<char*>(vCertDirectory.data()), uiSize); // reads the whole directory

	// Verify zeroed certificates (00002edec5247488029b2cc69568dda90714eeed8de0d84f1488635196b7e708)
	if (std::all_of(vCertDirectory.begin(), vCertDirectory.end(), [](unsigned char item) { return item == 0; }))
	{
		m_ldrError = LDR_ERROR_DIGITAL_SIGNATURE_ZEROED;
		return ERROR_INVALID_FILE;
	}

	InputBuffer inpBuffer(vCertDirectory);

	// 64-bit: cert.Length is a uint32 out of the file and this accumulates it,
	// so a 32-bit counter wraps and the loop restarts inside the same buffer.
	std::uint64_t bytesRead = 0;
	while (bytesRead < uiSize)
	{
		PELIB_IMAGE_CERTIFICATE_ENTRY cert;
		inpBuffer >> cert.Length;
		inpBuffer >> cert.Revision;
		inpBuffer >> cert.CertificateType;

		if ((cert.Length <= PELIB_IMAGE_CERTIFICATE_ENTRY::size()
			 || ((cert.Revision != PELIB_WIN_CERT_REVISION_1_0) && (cert.Revision != PELIB_WIN_CERT_REVISION_2_0))
			 || (cert.CertificateType != PELIB_WIN_CERT_TYPE_PKCS_SIGNED_DATA)))
		{
			return ERROR_INVALID_FILE;
		}

		// cert.Length is the WIN_CERTIFICATE Length field, straight out of the
		// file, and the checks above bound it below and not above. The resize
		// that follows trusted it: a 1,040-byte PE declaring Length=0x80000000
		// took the process from 4.2 MB to 4.2 GB of RSS, measured, and
		// 0xFFFFFFFF permits about twice that. The bytes have to come from the
		// directory that was actually read, so that is what bounds it --
		// compared in subtraction form, because `bytesRead + cert.Length` is
		// the wrap this loop already carries once.
		//
		// ResourceDirectory.cpp:337 fixed the same amplification class in this
		// file's sibling; this site was missed.
		const std::uint64_t payload = static_cast<std::uint64_t>(cert.Length) - PELIB_IMAGE_CERTIFICATE_ENTRY::size();
		if (payload > uiSize - bytesRead)
		{
			m_ldrError = LDR_ERROR_DIGITAL_SIGNATURE_CUT;
			return ERROR_INVALID_FILE;
		}

		cert.Certificate.resize(static_cast<std::size_t>(payload));
		inpBuffer.read(reinterpret_cast<char*>(cert.Certificate.data()), cert.Certificate.size());

		bytesRead += cert.Length;
		// Moved, not copied: the payload can be most of the directory, and
		// push_back(cert) doubled the peak for no reason.
		m_certs.push_back(std::move(cert));
	}

	// save the offset and size for future checks
	this->offset = uiOffset;
	this->size = size;

	return ERROR_NONE;
}

std::uint64_t SecurityDirectory::getOffset() const
{
	return offset;
}

std::uint64_t SecurityDirectory::getSize() const
{
	return size;
}
} // namespace PeLib
