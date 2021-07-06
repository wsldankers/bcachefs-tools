#include "../../libbcachefs/opts.h"

/**
 * generate tables from definitions in opt.h
 */

#define NULL (null)

FMT_START_SECTION

FMT_START_LINE Name ; Data Type ; Type Description ; Description ; Usage Flag FMT_END_LINE

#define x(_name, _shortopt, _type, _in_mem_type, _mode, _sb_opt, _idk1, _idk2)\
    FMT_START_LINE _name ; _shortopt ; _idk1 ; _idk2 ; _type FMT_END_LINE
    BCH_OPTS()
#undef x

FMT_END_SECTION
