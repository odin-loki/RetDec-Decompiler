/**
 * @file src/pdbparser/pdb_file.cpp
 * @brief PDB file.
 * @copyright (c) 2017 Avast Software, licensed under the MIT license
 * @copyright (c) 2025-2026 Odin Loch trading as Imortek (modifications)
 */

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "retdec/pdbparser/pdb_file.h"

using namespace std;

namespace retdec {
namespace pdbparser {

// =================================================================
// PUBLIC METHODS
// =================================================================

/**
 * Loads PDB file into memory and separates all streams.
 * Must be called before using of any method.
 * Can be called only once.
 * @param filename Name of PDB file to load.
 * @return Loading status
 */
PDBFileState PDBFile::load_pdb_file(const char* filename)
{
	if (pdb_loaded) return PDB_STATE_ALREADY_LOADED;

	// Load PDB file into memory
	pdb_filename = filename;
	FILE* fp = fopen(filename, "rb");
	if (fp == nullptr)
	{
		return PDB_STATE_ERR_FILE_OPEN;
	}
	fseek(fp, 0, SEEK_END); // Determine file size
	long file_size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	// An empty file has no header to compare against, and a size that does not
	// fit into pdb_file_size could not be used to bound the page numbers that
	// are read out of the file later on.
	if (file_size <= 0 || static_cast<unsigned long>(file_size) > UINT_MAX)
	{
		fclose(fp);
		return PDB_STATE_INVALID_FILE;
	}
	pdb_file_size = static_cast<unsigned int>(file_size);
	pdb_file_data = new char[pdb_file_size];                    // Allocate memory
	size_t result = fread(pdb_file_data, 1, pdb_file_size, fp); // Read the file
	fclose(fp);
	if (result != pdb_file_size)
	{
		return PDB_STATE_ERR_FILE_OPEN;
	}

	// Get the version of PDB file and parse it
	pdb_header = reinterpret_cast<PDB_HEADER*>(pdb_file_data);
	PDBFileState state;
	// Version is 2.00
	if (has_signature(PDB_SIGNATURE_200, sizeof(PDB_HEADER_200)))
	{
		pdb_version = PDB_VERSION_200;
		state = load_pdb_v200();
	}
	// Version is 7.00
	else if (has_signature(PDB_SIGNATURE_700, sizeof(PDB_HEADER_700)))
	{
		pdb_version = PDB_VERSION_700;
		state = load_pdb_v700();
		// Get pointer to PDB info header
		if (streams.size() > PDB_STREAM_PDB)
		{
			pdb_info_v700 = reinterpret_cast<PDBInfo70*>(streams[PDB_STREAM_PDB].data);
		}
		else
		{
			return PDB_STATE_INVALID_FILE;
		}
	}
	else // Invalid file
	{
		return PDB_STATE_INVALID_FILE;
	}
	if (state == PDB_STATE_OK) pdb_loaded = true;
	return state;
}

/**
 * Processes all PDB file streams and fills data containers.
 * Must be called after load_pdb_file() and before any getting and printing or dumping method.
 * Can be called only once.
 * @param image_base Base address of program's virtual memory.
 */
void PDBFile::initialize(uint64_t image_base)
{
	if (!pdb_loaded || streams.size() <= PDB_STREAM_TPI || pdb_initialized || streams.size() <= PDB_STREAM_DBI)
	{
		return;
	}

	// Initialize types
	pdb_types = new PDBTypes(&streams[PDB_STREAM_TPI]);
	pdb_types->parse_types();

	// Check if DBI stream is present. Its header is read unconditionally below,
	// so a stream too short to hold that header describes no debug info at all.
	bool dbi_present =
		(num_streams > PDB_STREAM_DBI && streams[PDB_STREAM_DBI].unused == false
		 && streams[PDB_STREAM_DBI].size >= static_cast<int>(sizeof(NewDBIHdr)));

	if (dbi_present)
	{
		// Get DBI stream
		unsigned int pdb_dbi_size = streams[PDB_STREAM_DBI].size;
		char* pdb_dbi_data = streams[PDB_STREAM_DBI].data;

		// Get pointer to DBI header
		dbi_header_v700 = reinterpret_cast<NewDBIHdr*>(pdb_dbi_data);

		// Get debug stream numbers
		// cbDbgHdr is the file's claim about how many bytes of debug stream
		// numbers sit at the end of the DBI stream. It was subtracted from that
		// end and indexed without ever being compared to the stream it is part
		// of, so a made up length addressed memory before the stream, and a
		// short list of numbers was read past its end.
		// The numbers are 16 bit, so the list starts at an even offset of the
		// stream; a length that puts it on an odd one does not describe one.
		PDB_LONG dbg_hdr_size = dbi_header_v700->cbDbgHdr;
		if (dbg_hdr_size > 0 && static_cast<unsigned int>(dbg_hdr_size) <= pdb_dbi_size - sizeof(NewDBIHdr)
			&& (pdb_dbi_size - dbg_hdr_size) % sizeof(PDB_SHORT) == 0)
		{
			int dbg_count = dbg_hdr_size / sizeof(PDB_SHORT);
			PDB_SHORT* dbg_numbers = reinterpret_cast<PDB_SHORT*>(pdb_dbi_data + pdb_dbi_size - dbg_hdr_size);
			if (dbg_count > 0) pdb_fpo_num = dbg_numbers[0];
			if (dbg_count > 5) pdb_sec_num = dbg_numbers[5];
			if (dbg_count > 9) pdb_newfpo_num = dbg_numbers[9];
		}

		// Initialize modules
		parse_modules();

		// Intialize sections
		if (image_base == 0) image_base = 0x400000; // Default image base
		parse_sections(image_base);

		// Initialize symbols
		int pdb_gsi_num = dbi_header_v700->snGSSyms;
		int pdb_psi_num = dbi_header_v700->snPSSyms;
		int pdb_sym_num = dbi_header_v700->snSymRecs;
		// The three stream numbers come from the DBI header; each was used to
		// index the stream vector directly, which reads past its end whenever
		// the file names a stream that this PDB does not contain (0xffff, the
		// "no stream" marker, included).
		if (stream_num_is_valid(pdb_gsi_num) && stream_num_is_valid(pdb_psi_num) && stream_num_is_valid(pdb_sym_num))
		{
			pdb_symbols = new PDBSymbols(
				&streams[pdb_gsi_num], &streams[pdb_psi_num], &streams[pdb_sym_num], modules, sections, pdb_types);
			pdb_symbols->parse_symbols();
		}
	}
	pdb_initialized = true;
}

/**
 * Saves all streams into separate files.
 * File names consist of input PDB file name and extension .xxx as stream number
 * Can be called after load_pdb_file() was executed
 * @return Operation was successful
 */
bool PDBFile::save_streams_to_files(void)
{
	if (!pdb_loaded || num_streams == 0) return false;
	// Save each stream to file
	for (unsigned int i = 0; i < num_streams; i++)
	{
		char stream_filename[MAX_PATH + 4];
		std::snprintf(stream_filename, sizeof(stream_filename), "%s.%03d", pdb_filename, i);
		FILE* fs = fopen(stream_filename, "wb");
		if (fs == nullptr) return false;
		if (!streams[i].unused) fwrite(streams[i].data, 1, streams[i].size, fs);
		fclose(fs);
	}
	return true;
}

/**
 * Prints basic PDB file information and list of streams
 * Can be called after load_pdb_file() was executed
 */
void PDBFile::print_pdb_file_info(void)
{
	puts("******* PDB file info *******");
	if (!pdb_loaded)
	{
		puts("PDB file not properly loaded yet!\n");
		return;
	}
	printf("File name: %s\n", pdb_filename);
	printf("File size: %d bytes \n", pdb_file_size);
	printf("PDB version: ");
	if (pdb_version == PDB_VERSION_200)
		printf("2.00\n");
	else if (pdb_version == PDB_VERSION_700)
		printf("7.00\n");
	if (pdb_info_v700 != nullptr)
	{
		printf("Age: %d\n", pdb_info_v700->pdbinfo.age);
		printf("GUID: ");
		print_bytes(reinterpret_cast<PDB_BYTE*>(&pdb_info_v700->sig70), sizeof(PDB_GUID));
		puts("");
	}
	printf("Page size: 0x%x bytes\n", page_size);
	printf("Number of streams: %d\n", num_streams);
	for (unsigned int i = 0; i < num_streams; i++)
		printf(
			"Stream %02d size: %7d unused: %d linear: %d\n", i, streams[i].size, streams[i].unused, streams[i].linear);

	puts("");
}

/**
 * Prints all modules names and their stream numbers
 * Can be called after initialize() was executed
 */
void PDBFile::print_modules(void)
{
	puts("******* PDB list of modules *******");
	if (!pdb_initialized)
	{
		puts("PDB file not initialized yet!\n");
		return;
	}
	if (modules.size() == 0)
	{
		puts("PDB file doesn't contain module list!\n");
		return;
	}
	printf("Symbol streams:\n");
	printf("  GSI stream: %d\n", dbi_header_v700->snGSSyms);
	printf("  PSGSI stream: %d\n", dbi_header_v700->snPSSyms);
	printf("  SYM stream: %d\n", dbi_header_v700->snSymRecs);
	puts("");
	printf("List of modules:\n");
	for (unsigned int i = 0; i < modules.size(); i++)
		printf("  Stream number: %d Module name: %s\n", modules[i].stream_num, modules[i].name);
	puts("");
}

/**
 * Dumps FPO stream.
 * Can be called after initialize() was executed.
 */
void PDBFile::dump_FPO(void)
{
	puts("******* FPO dump *******");
	if (!pdb_initialized)
	{
		puts("PDB file not initialized yet!\n");
		return;
	}
	if (pdb_fpo_num <= 0 || !stream_num_is_valid(pdb_fpo_num))
	{
		puts("FPO information not present in PDB file!\n");
		return;
	}

	PDBStream* pdb_fpo_stream = &streams[pdb_fpo_num];
	int fpoSize = pdb_fpo_stream->size;
	PDB_FPO_DATA* fpo = reinterpret_cast<PDB_FPO_DATA*>(pdb_fpo_stream->data);

	int fpoCount = fpoSize / sizeof(PDB_FPO_DATA);
	for (int i = 0; i < fpoCount; i++)
	{
		printf(
			"start %08x size %08x locals %08x params %04x "
			"prolog %02x regs %x SEH? %x EBP? %x rsvd %x frameType %x\n",
			fpo[i].ulOffStart,
			fpo[i].cbProcSize,
			fpo[i].cdwLocals,
			fpo[i].cdwParams,
			fpo[i].cbProlog,
			fpo[i].cbRegs,
			fpo[i].fHasSEH,
			fpo[i].fUseBP,
			fpo[i].reserved,
			fpo[i].cbFrame);
	}
	puts("");
}

/**
 * Dumps PE Sections stream.
 * Can be called after initialize() was executed.
 */
void PDBFile::dump_PE_sections(void)
{
	puts("******* PE sections dump *******");
	if (!pdb_initialized)
	{
		puts("PDB file not initialized yet!\n");
		return;
	}
	if (pdb_sec_num <= 0 || !stream_num_is_valid(pdb_sec_num))
	{
		puts("PE sections information not present in PDB file!\n");
		return;
	}

	PDBStream* pdb_sect_stream = &streams[pdb_sec_num];
	PDB_PVOID pSect = pdb_sect_stream->data;
	unsigned long sectSize = pdb_sect_stream->size;

	int nSect = sectSize / sizeof(PDB_IMAGE_SECTION_HEADER);
	PDB_PIMAGE_SECTION_HEADER Sections = reinterpret_cast<PDB_PIMAGE_SECTION_HEADER>(pSect);
	for (int i = 0; i < nSect; i++)
	{
		// The name is a fixed size field that is not required to be terminated,
		// so it is printed as the eight bytes it is.
		printf(
			"%.8s (VA %08x * %08x RawSize %08x Misc %08x)\n",
			Sections[i].Name,
			Sections[i].VirtualAddress,
			Sections[i].PointerToRawData,
			Sections[i].SizeOfRawData,
			Sections[i].Misc.VirtualSize);
	}
	puts("");
}

/**
 * Destructor
 */
PDBFile::~PDBFile()
{
	if (pdb_file_data) delete[] pdb_file_data;
	// A non-linear root directory was copied into memory of its own
	if (pdb_root_dir && !pdb_root_dir_linear) delete[] reinterpret_cast<char*>(pdb_root_dir);
	// Delete all non-linear (copied) streams
	for (unsigned int i = 0; i < num_streams; i++)
		if (!streams[i].unused && !streams[i].linear) delete[] streams[i].data;
	if (pdb_types) delete pdb_types;
	if (pdb_symbols) delete pdb_symbols;
}

// =================================================================
// PRIVATE METHODS
// =================================================================

/**
 * Checks that the loaded file starts with the given version signature and is
 * large enough to hold that version's header.
 * The signature used to be matched with strcmp(), which assumes the file image
 * is terminated somewhere: a file shorter than the signature let the comparison
 * run off the end of the buffer the file was read into. The header size is
 * checked here as well, because load_pdb_v200() and load_pdb_v700() read their
 * header without looking at the file size again.
 * @param signature Version signature, matched including its terminating zero
 * @param header_size Size of the version's file header
 * @return File starts with the signature and can hold the header
 */
bool PDBFile::has_signature(const char* signature, unsigned int header_size)
{
	size_t signature_size = strlen(signature) + 1;
	if (pdb_file_size < signature_size || pdb_file_size < header_size) return false;
	return memcmp(pdb_file_data, signature, signature_size) == 0;
}

/**
 * Determines whether a run of pages lies inside the loaded file.
 * Page numbers and page counts are read out of the file, so nothing guarantees
 * that they name bytes which were actually loaded from disk.
 * @param first_page Number of the first page of the run
 * @param num_pages Number of pages in the run
 * @return Whole run is inside the loaded file
 */
bool PDBFile::pages_in_file(uint64_t first_page, uint64_t num_pages)
{
	// Both arguments come from 32 bit fields and page_size is at most 0x1000,
	// so this cannot overflow the 64 bit arithmetic.
	return (first_page + num_pages) * page_size <= pdb_file_size;
}

/**
 * Determines whether a stream number read from the file names an existing stream.
 * Every stream number in a PDB is a 16 bit field, and the format reserves
 * 0xffff in all of them to say "this stream is not here". The range check alone
 * usually rejects it, because a file rarely has that many streams -- but a file
 * that does would have turned the marker back into an index, which is not what
 * it means.
 * @param num Stream number
 * @return Stream with this number is present in the file
 */
bool PDBFile::stream_num_is_valid(int num)
{
	return num >= 0 && num != PDB_STREAM_NONE && static_cast<unsigned int>(num) < num_streams;
}

/**
 * Determines whether stream is stored linear in PDB file or not
 * @param pages Index of pages used by stream
 * @param num_pages Number of pages used by stream
 * @return Stream is linear
 */
bool PDBFile::stream_is_linear(PDB_DWORD* pages, int num_pages)
{
	PDB_DWORD cur_page = pages[0];
	for (int i = 1; i < num_pages; i++)
		if (pages[i] != ++cur_page) return false;
	return true;
}

/**
 * Extracts non-linear stream into linear memory.
 * Every page number must have been checked with pages_in_file() by the caller,
 * this copies the pages it is given.
 * @param pages Index of pages used by stream
 * @param num_pages Number of pages used by stream
 * @return Stream data in linear memory
 */
char* PDBFile::extract_stream(PDB_DWORD* pages, int num_pages)
{
	// Copy data from each page
	char* stream_data = new char[num_pages * page_size];
	for (int i = 0; i < num_pages; i++)
	{
		memcpy(stream_data + page_size * i, pdb_file_data + pages[i] * page_size, page_size);
	}
	return stream_data;
}

/**
 * Separates all streams from PDB file version 2.00.
 * Vector "streams" is filled here.
 * @return State (OK or Invalid file)
 */
PDBFileState PDBFile::load_pdb_v200(void)
{
	// TODO - add support for PDB version 2.00
	return PDB_STATE_UNSUPPORTED_VERSION;
}

/**
 * Separates all streams from PDB file version 7.00.
 * Vector "streams" is filled here.
 * @return State (OK or Invalid file)
 */
PDBFileState PDBFile::load_pdb_v700(void)
{
	// Get page size
	page_size = pdb_header->V700.dBytesPerPage;
	if (!(page_size == 0x200 || page_size == 0x400 || page_size == 0x800 || page_size == 0x1000))
		return PDB_STATE_INVALID_FILE;

	// Check file size
	// The product is computed in 64 bits because a page count that makes it
	// wrap would pass a check whose whole purpose is to tie the page numbers
	// below to bytes that are really there.
	if (pdb_file_size != static_cast<uint64_t>(page_size) * pdb_header->V700.dNumPages) return PDB_STATE_INVALID_FILE;

	// Get root directory
	// The root directory is stored in pages of this file and has to hold at
	// least the stream count, and the page numbers listing it are themselves
	// stored in a single page: a directory needing more page numbers than fit
	// into one page cannot be described by an MSF file at all.
	unsigned int root_size = pdb_header->V700.dRootSize;
	if (root_size < sizeof(PDB_DWORD) || root_size > pdb_file_size) return PDB_STATE_INVALID_FILE;
	int pages_per_root = (static_cast<uint64_t>(root_size) + page_size - 1) / page_size;
	if (static_cast<unsigned int>(pages_per_root) > page_size / sizeof(PDB_DWORD)
		|| !pages_in_file(pdb_header->V700.dRootIndexesPage, 1))
		return PDB_STATE_INVALID_FILE;
	PDB_DWORD* root_dir_indexes =
		reinterpret_cast<PDB_DWORD*>(pdb_file_data + (pdb_header->V700.dRootIndexesPage) * page_size);
	// Each page of the directory is either pointed into or copied out of the
	// file image, so each of these page numbers has to name a page of it.
	for (int i = 0; i < pages_per_root; i++)
		if (!pages_in_file(root_dir_indexes[i], 1)) return PDB_STATE_INVALID_FILE;
	if (stream_is_linear(root_dir_indexes, pages_per_root))
	{
		pdb_root_dir = reinterpret_cast<PDB_ROOT*>(pdb_file_data + root_dir_indexes[0] * page_size);
		pdb_root_dir_linear = true;
	}
	else
	{
		pdb_root_dir = reinterpret_cast<PDB_ROOT*>(extract_stream(root_dir_indexes, pages_per_root));
		pdb_root_dir_linear = false;
	}

	// Get streams
	// Number of dwords the root directory holds after the stream count. Every
	// stream costs at least the dword with its size, so a file claiming more
	// streams than the directory can describe is malformed by construction;
	// the count used to be believed and the vector below sized for it. It is
	// stored only once it is known to be the number of streams we really have,
	// because the destructor walks the vector with it.
	unsigned int root_dwords = root_size / sizeof(PDB_DWORD) - 1;
	if (pdb_root_dir->V700.dNumStreams > root_dwords) return PDB_STATE_INVALID_FILE;
	num_streams = pdb_root_dir->V700.dNumStreams;
	// Allocate memory for streams. We need to use resize() instead of
	// reserve() because reserve() does not increases the size of the
	// container. That would make accesses to it in the following loop invalid.
	streams.resize(num_streams);
	int cur_pagedir_index = num_streams + 0; // Skip dwords with stream sizes

	// Extract each stream
	for (unsigned int i = 0; i < num_streams; i++)
	{
		streams[i].size = pdb_root_dir->V700.adStreamSizes[i];
		// Stream is empty
		if (streams[i].size <= 0)
		{
			streams[i].unused = true;
			streams[i].linear = false;
			streams[i].data = nullptr;
		}
		// Stream is not empty
		else
		{
			streams[i].unused = false;
			// A stream is stored in pages of this file, so it cannot be bigger
			// than the file itself.
			if (static_cast<unsigned int>(streams[i].size) > pdb_file_size) return PDB_STATE_INVALID_FILE;
			int pages_per_stream = (static_cast<uint64_t>(streams[i].size) + page_size - 1) / page_size;
			// The page numbers of all streams follow the sizes in the same
			// directory, and every page has to be one that was read from disk:
			// both were taken from the file and used unchecked, which walked
			// off the directory and then off the file image.
			if (static_cast<unsigned int>(cur_pagedir_index) + pages_per_stream > root_dwords)
				return PDB_STATE_INVALID_FILE;
			PDB_DWORD* stream_pages = &pdb_root_dir->V700.adStreamSizes[cur_pagedir_index];
			for (int p = 0; p < pages_per_stream; p++)
				if (!pages_in_file(stream_pages[p], 1)) return PDB_STATE_INVALID_FILE;
			// Stream is linear in pdb file, we just get a pointer to it
			if (stream_is_linear(stream_pages, pages_per_stream))
			{
				streams[i].data = pdb_file_data + stream_pages[0] * page_size;
				streams[i].linear = true;
			}
			// Stream is not linear in pdb file, we must copy it to linear memory
			else
			{
				streams[i].data = extract_stream(stream_pages, pages_per_stream);
				streams[i].linear = false;
			}
			cur_pagedir_index += pages_per_stream; // Increase index to next stream
		}
	}
	return PDB_STATE_OK;
}

/**
 * Parses DBI stream and gets names of modules and their streams.
 * Vector "modules" is filled here.
 */
void PDBFile::parse_modules(void)
{
	// Get DBI stream size and data
	PDBStream* pdb_dbi_stream = &streams[PDB_STREAM_DBI];
	unsigned int pdb_dbi_size = pdb_dbi_stream->size;
	char* pdb_dbi_data = pdb_dbi_stream->data;

	if (pdb_dbi_size < sizeof(NewDBIHdr)) // DBI stream is empty
		return;

	// cbGpModi is the file's claim about how many bytes of module records
	// follow the header. The module list is part of the DBI stream, so it
	// cannot reach past it; it used to be trusted and walked to that end.
	if (dbi_header_v700->cbGpModi < 0
		|| static_cast<unsigned int>(dbi_header_v700->cbGpModi) > pdb_dbi_size - sizeof(NewDBIHdr))
		return;

	unsigned int position = sizeof(NewDBIHdr); // 0x40
	unsigned int limit = sizeof(NewDBIHdr) + dbi_header_v700->cbGpModi;
	int cnt = 0;
	MODI* entry;

	// A record that does not fit into the module list in full is truncated,
	// not a module.
	while (position + sizeof(MODI) <= limit)
	{
		// Parse entries with module information
		entry = reinterpret_cast<MODI*>(pdb_dbi_data + position);

		// Determine the end of entry
		int len = 0;
		bool ended = false;          // String already ended
		int second_len = 0;          // Length of second string
		bool ends_in_stream = false; // Record ends inside the DBI stream
		// The scan stops on the terminators the record is supposed to contain,
		// so the DBI stream holding it is what bounds it. Nothing bounded it
		// before, and a record whose names are not terminated walked out of the
		// stream and off the file image behind it.
		unsigned int names_max = pdb_dbi_size - position - sizeof(MODI);
		while (static_cast<unsigned int>(len) < names_max)
		{
			if (entry->rgch[len] == 0)
			{
				if (ended && second_len > 1 && (len & 3) == 0)
				{
					ends_in_stream = true;
					break;
				}
				ended = true;
				second_len++;
			}
			else
				ended = false;
			len++;
		}
		if (!ends_in_stream) break;

		// Add module into vector
		// PDB_STREAM_NONE means that the module has no stream; any other number
		// has to name a stream this file really contains, indexing the vector
		// with it used to be done on the file's word alone.
		PDBStream* s = (entry->sn == PDB_STREAM_NONE || !stream_num_is_valid(entry->sn))
						 ? nullptr
						 : &streams[entry->sn]; // Get module stream
		PDBModule new_module = {
			reinterpret_cast<char*>(entry->rgch), // name
			entry->sn,                            // stream_num
			s                                     // stream
		};
		modules.push_back(new_module);
		cnt++;
		// Go to next entry
		position += sizeof(MODI) + len;
	}
}

/**
 * Parses PE Sections stream and gets section name, virtual address and file address
 * Vector "sections" is filled here.
 */
void PDBFile::parse_sections(uint64_t image_base)
{
	// The stream number comes from the DBI debug header, so nothing but this
	// check keeps it from naming a stream the file does not have.
	if (pdb_sec_num <= 0 || !stream_num_is_valid(pdb_sec_num)) // Sections stream not present
		return;

	// Get stream with section info
	PDBStream* pdb_sect_stream = &streams[pdb_sec_num];
	unsigned int pdb_sect_size = pdb_sect_stream->size;
	char* pdb_sect_data = pdb_sect_stream->data;

	// A stream can be in range and still carry no data -- an unused or
	// unreadable one has data == nullptr -- while its recorded size is
	// non-zero, so the count below would be positive and the loop would walk a
	// null pointer.
	if (pdb_sect_data == nullptr) return;

	// Get number of sections and array of section headers
	int num_sects = pdb_sect_size / sizeof(PDB_IMAGE_SECTION_HEADER);
	PDB_IMAGE_SECTION_HEADER* sects = reinterpret_cast<PDB_IMAGE_SECTION_HEADER*>(pdb_sect_data);

	// Create dummy zero-number section
	PDBPESection zero_sect = {"", 0, 0};
	sections.push_back(zero_sect);

	int max_code_sect = 1; // Maximum number of code section
	// Parse all sections
	for (int i = 0; i < num_sects; i++)
	{
		PDBPESection new_sect = {
			reinterpret_cast<char*>(sects[i].Name), // name
			sects[i].VirtualAddress + image_base,   // virtual_address
			sects[i].PointerToRawData               // file_address
		};
		sections.push_back(new_sect);
		// Check if section is code section
		if (strncmp(reinterpret_cast<const char*>(sects[i].Name), ".text", 5) == 0) max_code_sect = i + 1;
	}
	sections[0].file_address = max_code_sect;
}

} // namespace pdbparser
} // namespace retdec
