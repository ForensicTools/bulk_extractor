/**
 * A simple regex finder.
 */

#include "config.h"

#include "be13_api/scanner_params.h"
#include "be13_api/regex_vector.h"
#include "be13_api/utils.h" // needs config.h
#include "findopts.h"
#include "managed_malloc.h"

//#include "histogram.h"

//#include "bulk_extractor.h" // for regex_list type

// anonymous namespace hides symbols from other cpp files (like "static" applied to functions)
namespace {
    regex_vector find_list;
    void add_find_pattern(const std::string &pat) {
        find_list.push_back("(" + pat + ")"); // make a group
    }

    void process_find_file(const char *findfile) {
        std::ifstream in;

        in.open(findfile,std::ifstream::in);
        if(!in.good()) {
            std::cerr << "Cannot open " << findfile << "\n";
            throw std::runtime_error(findfile);
        }
        while(!in.eof()){
            std::string line;
            getline(in,line);
            truncate_at(line,'\r');         // remove a '\r' if present
            if(line.size()>0) {
                if(line[0]=='#') continue;  // ignore lines that begin with a comment character
                add_find_pattern(line);
            }
        }
    }
}

extern "C"
void scan_find(scanner_params &sp)
{
    sp.check_version();
    if(sp.phase==scanner_params::PHASE_INIT) {
        sp.info = new scanner_params::scanner_info(scan_find,"find");
        sp.info->name		= "find";
        sp.info->author         = "Simson Garfinkel";
        sp.info->description    = "Simple search for patterns";
        sp.info->scanner_version= "1.1";
        //sp.info->flags		= scanner_info::SCANNER_FIND_SCANNER;
        sp.info->feature_defs.push_back( feature_recorder_def("find"));
        auto lowercase = histogram_def::flags_t(); lowercase.lowercase = true;
      	sp.info->histogram_defs.push_back( histogram_def("find", "find", "", "","histogram", lowercase));
        return;
    }
    if(sp.phase==scanner_params::PHASE_SHUTDOWN) return;

    if (scanner_params::PHASE_INIT == sp.phase) {
        for (auto const &it : FindOpts::get().Patterns) {
            add_find_pattern(it);
        }
        for (auto const &it : FindOpts::get().Files) {
            process_find_file(it.c_str());
        }
    }

    if(sp.phase==scanner_params::PHASE_SCAN) {
        /* The current regex library treats \0 as the end of a string.
         * So we make a copy of the current buffer to search that's one bigger, and the copy has a \0 at the end.
         * This is not efficient, but we're stuck with it.
         */
        feature_recorder &f = sp.ss.named_feature_recorder("find");

        managed_malloc<u_char> tmpbuf(sp.sbuf->bufsize+1);
        if(!tmpbuf.buf) return;				     // no memory for searching
        memcpy(tmpbuf.buf, sp.sbuf->buf, sp.sbuf->bufsize);    // copy the data in
        tmpbuf.buf[sp.sbuf->bufsize]=0;                       // null terminate
        for (size_t pos = 0; pos < sp.sbuf->pagesize && pos < sp.sbuf->bufsize;) {
            /* Now see if we can find a string */
            std::string found;
            size_t offset=0;
            size_t len = 0;
            if ( find_list.search_all((const char *)tmpbuf.buf+pos, &found, &offset, &len)) {
                if(len == 0) {
                    len+=1;
                    continue;
                }
                f.write_buf(*sp.sbuf,pos+offset,len);
                pos += offset+len;
            } else {
                /* nothing was found; skip past the first \0 and repeat. */
                const u_char *eos = (const u_char *)memchr(tmpbuf.buf+pos,'\000',sp.sbuf->bufsize-pos);
                if(eos) pos=(eos-tmpbuf.buf)+1;		// skip 1 past the \0
                else    pos=sp.sbuf->bufsize;	// skip to the end of the buffer
            }
        }
    }
}
