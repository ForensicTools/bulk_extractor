/**
 * \addtogroup winprefetch_scanner
 * @{
 */
/**
 * \file
 *
 * scan_winprefetch finds windows prefetch entries.
 *
 * Revision history:
 * - Created by Luis E. Garcia II
 * - 2012-apr-27 bda - Optimized for performance and to fix prefetch entries
 *                   that were missed when the scanner ran on Windows.
 *
 * Resources:
 * - https://forensicswiki.xyz/wiki/index.php?title=Prefetch
 * - https://forensicswiki.xyz/wiki/index.php?title=Windows_Prefetch_File_Format
 * - prefetch research discussion: http://42llc.net/?p=375
 * - a viewer: http://www.nirsoft.net/utils/win_prefetch_view.html
 * - Use of UTF16 in Windows OS: http://en.wikipedia.org/wiki/UTF-16
 */

#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <sstream>
#include <vector>


#include "config.h"
#include "utf8.h"
#include "be13_api/utils.h"             // for safe_utf16to8, requires config.h
#include "be13_api/scanner_params.h"
#include "be13_api/sbuf_stream.h"
#include "dfxml_cpp/src/dfxml_writer.h"     // requires config.h

/**
 * Instantiates a populated prefetch record from the buffer provided.
 */
struct prefetch_record_t {
    bool isvalid {false};
    std::string prefetch_version {};
    uint32_t header_size         {};
    std::string   execution_filename {};
    uint32_t execution_counter   {};
    int64_t  execution_time      {};
    std::string   volume_path_name {};
    uint32_t volume_serial_number  {};
    int64_t  volume_creation_time  {};
    std::vector<std::string> files {};		// files in prefect record
    std::vector<std::string> directories {};	// directories in prefect

    std::string to_xml();		// turns the record to an XML

    prefetch_record_t(){};
    bool valid_full_path_name(const std::string &str);
    bool validate(const sbuf_t &sbuf);  // returns true if sbuf points to a validate record
};

bool prefetch_record_t::valid_full_path_name(const std::string &str)
{
    if (str.size()<2) return false;
    if (str.at(0)!='\\') return false;
    for (size_t i=0;i<str.size();i++){
        if ((uint8_t)str.at(i)=='\000') return false; // no null characters in UTF-8
    }
    /* Additional checks? */
    return true;
}

bool prefetch_record_t::validate(const sbuf_t &sbuf)
{
    // read fields in order until done or range exception
    try {

        // get prefetch version identifier
        isvalid = false;                  // until we know otherwise
        uint8_t prefetch_version_byte   = sbuf.get8u(0);

        // set values based on prefetch version
        size_t execution_time_offset    = 0;
        size_t execution_counter_offset = 0;

        if (prefetch_version_byte == 0x11) {
            prefetch_version = "Windows XP";
            execution_time_offset = 0x78;
            execution_counter_offset = 0x90;
            isvalid = true;
        } else if (prefetch_version_byte == 0x17) {
            prefetch_version = "Windows Vista or Windows 7";
            execution_time_offset = 0x80;
            execution_counter_offset = 0x98;
            isvalid = true;
        } else {
            // program error: don't create prefetch_record if this byte is invalid.
            // This was an assert(0), but let's just return
            return isvalid;               // valid is false
        }

        // size in bytes of the whole prefetch file
        uint32_t prefetch_file_length = sbuf.get32u(0x0c);

        // get execution file filename
        std::wstring utf16_execution_filename = sbuf.getUTF16(0x10);
        execution_filename = safe_utf16to8(utf16_execution_filename);
        if (execution_filename.size()==0) execution_filename="UNKNOWN_FILENAME";

        // get the offset to Section A
        uint32_t section_a_offset = sbuf.get32u(0x54);
        header_size = section_a_offset; // header is everything before section A

        // validate the offset since we know what it should be
        if ((prefetch_version_byte == 0x11 && header_size != 0x98)		// XP and 2003
            || (prefetch_version_byte == 0x17 && header_size != 0xf0)) {	// Vista and 7
            // invalid so quit trying
            isvalid = false;
            return isvalid;
        }

        execution_time = sbuf.get64u(execution_time_offset);
        execution_counter = sbuf.get32u(execution_counter_offset);

        // get the list of files from Section C
        uint32_t section_c_offset = sbuf.get32u(0x64);
        uint32_t section_c_length = sbuf.get32u(0x68);
        sbuf_stream filename_stream( sbuf.slice(section_c_offset));
        while (filename_stream.tell() < section_c_length) {
            std::cerr << "section_c_offset=" << section_c_offset << " section_c_length=" << section_c_length << " filename_stream.tell()=" << filename_stream.tell() << " sbuf=" << sbuf << "\n";
            std::wstring utf16_filename = filename_stream.getUTF16();
            std::string filename = safe_utf16to8(utf16_filename);
            if (!valid_full_path_name(filename)) return isvalid;
            files.push_back(filename);
        }

        // Process Section D
        uint32_t section_d_offset = sbuf.get32u(0x6c);

        uint32_t volume_name_offset = sbuf.get32u(section_d_offset + 0x00);
        std::wstring utf16_volume_name = sbuf.getUTF16(section_d_offset+volume_name_offset);
        volume_path_name = safe_utf16to8(utf16_volume_name);

        volume_creation_time = sbuf.get64i(section_d_offset+0x08);
        volume_serial_number = sbuf.get32u(section_d_offset+0x10);

        uint32_t section_d_2_offset = sbuf.get32u(section_d_offset + 0x1c);
        size_t   directory_offset = section_d_offset + section_d_2_offset;
        uint32_t num_directory_entries = sbuf.get32u(section_d_offset + 0x20);

        // get each of the directory entries from Section D subsection 2
        if (directory_offset > prefetch_file_length) {
            // the offset is out of range so don't get the list of directories
        } else {
            // calculate a rough maximum number of bytes for directory entries
            size_t      upper_max = prefetch_file_length - directory_offset;
            sbuf_stream directory_stream = sbuf_stream( sbuf.slice(directory_offset));

            for (uint32_t i=0; i<num_directory_entries; i++) {
                // break if obviously out of range
                if (directory_stream.tell() > upper_max) {
                    return isvalid;		// rest of data not good
                }

                // for directories, the first int16 is the directory name length.
                // We read to \U0000 instead so we throw away the directory name length.
                directory_stream.get16u();

                // read the directory name
                std::wstring utf16_directory_name = directory_stream.getUTF16();
                std::string directory_name = safe_utf16to8(utf16_directory_name);
                if (!valid_full_path_name(directory_name)) return isvalid;
                directories.push_back(directory_name);
            }
        }
        return isvalid;
    } catch (sbuf_t::range_exception_t &e) {
        // no action, just return what was gleaned before range exception
        return isvalid;
    }
}


/**
 * Returns an XML string from the prefetch record provided.
 */
// Private helper functions; turn a prefect record into an XML string */
std::string prefetch_record_t::to_xml()
{
    std::stringstream ss;

    if (!isvalid) {
        return "";
    }

    // generate the prefetch feature in a stringstream
    ss << "<prefetch>";
    ss << "<os>"       << dfxml_writer::xmlescape(prefetch_version) << "</os>";
    ss << "<filename>" << dfxml_writer::xmlescape(execution_filename) << "</filename>";
    ss << "<header_size>" << header_size << "</header_size>";
    ss << "<atime>"    << microsoftDateToISODate(execution_time) << "</atime>";
    ss << "<runs>"     << execution_counter << "</runs>";
    ss << "<filenames>";
    for (const auto &it : files) {
        ss << "<file>" << dfxml_writer::xmlescape(it) << "</file>";
    }
    ss << "</filenames>";

    ss << "<volume>";
    ss << "<path>" << dfxml_writer::xmlescape(volume_path_name) << "</path>";
    ss << "<creation>" << microsoftDateToISODate(volume_creation_time) << "</creation>";
    ss << "<serial_number>" << std::hex << volume_serial_number << std::dec << "</serial_number>";

    ss << "<dirnames>";
    for (const auto &it:directories) {
        ss << "<dir>" << dfxml_writer::xmlescape(it) << "</dir>";
    }
    ss << "</dirnames>";
    ss << "</volume>";
    ss << "</prefetch>";

    // return the xml as a string
    std::string prefetch_xml = ss.str();
    return prefetch_xml;
}

/**
 * Scanner scan_winprefetch scans and extracts windows prefetch records.
 * It takes scanner_params and recursion_control_block as input.
 * The scanner_params structure includes the following:
 * \li scanner_params::phase identifying the scanner phase.
 * \li scanner_params.sbuf containing the buffer being scanned.
 * \li scanner_params.fs, which provides feature recorder feature_recorder
 * that scan_winprefetch will write to.
 *
 * scan_winprefetch iterates through each byte of sbuf
 * searching for a valid winprefetch match.
 * When a match is found, the prefetch content is extracted, formatted for XML output,
 * and written to the windows prefetch feature recorder feature_recorder
 * using prefetch_record.write(sbuf_t, string, string).
 * Value sbuf_t is used to write the feature's path.
 * The first string is the feature found, in this case the windows prefetch filename is used.
 * The second string is the full feature content, in this case, packed in XML.
 * Method dfxml_writer::xml_escape() is used to help format XML output.
 */
feature_recorder *winprefetch_recorder = nullptr;
extern "C"
void scan_winprefetch(scanner_params &sp)
{
    sp.check_version();
    if (sp.phase==scanner_params::PHASE_INIT){
        sp.info->set_name("winprefetch");
        sp.info->name		= "winprefetch";
        sp.info->author		= "Bruce Allen";
        sp.info->description	= "Search for Windows Prefetch files";
        sp.info->feature_defs.push_back( feature_recorder_def("winprefetch"));
        sp.info->min_sbuf_size = 64;
        return;
    }
    if (sp.phase==scanner_params::PHASE_INIT2){
	winprefetch_recorder = &sp.named_feature_recorder("winprefetch");
    }
    if (sp.phase==scanner_params::PHASE_SCAN){

	// phase 1: set up the feature recorder and search for winprefetch features
	const sbuf_t &sbuf = *(sp.sbuf);

	size_t stop = (sbuf.pagesize > sbuf.bufsize + 8) ? sbuf.bufsize : sbuf.pagesize - 8;

	// iterate through sbuf searching for winprefetch features
        prefetch_record_t prefetch_record;
	for (size_t start=0; start < stop; start++) {

	    // check for probable WindowsXP or Windows7 header
	    if ((sbuf[start + 0] == 0x11 || sbuf[start + 0] == 0x17)
		&& sbuf[start + 1] == 0x00
		&& sbuf[start + 2] == 0x00
		&& sbuf[start + 3] == 0x00
		&& sbuf[start + 4] == 0x53
		&& sbuf[start + 5] == 0x43
		&& sbuf[start + 6] == 0x43
		&& sbuf[start + 7] == 0x41) {

		// create the populated prefetch record and see if it validates
                if (prefetch_record.validate(sbuf)) {
                    // record the winprefetch entry
                    winprefetch_recorder->write(sbuf.pos0+start, prefetch_record.execution_filename, prefetch_record.to_xml());
                    /* Should really skip to the end of the record we just
                     * parsed, but it's not immediately obvious how to get
                     * that info at this point.
                     */
                }
	    }
	}
    }
}
