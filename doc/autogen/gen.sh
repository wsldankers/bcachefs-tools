#!/bin/sh
# Pull options from opts.h into a csv file for generating documentation

$CC doc/autogen/gen.h -I libbcachefs -I include -E 2>/dev/null  \
	| sed -n '/FMT_START_SECTION/,/FMT_END_SECTION/p'       \
	| tr '\n' ' '                                           \
	| sed -e 's/FMT_START_LINE/\n/g;' 			\
		-e 's/FMT_END_LINE//g;' 			\
		-e 's|\\n||g;'					\
		-e 's/"//g;'					\
		-e 's/OPT_//g;' 				\
		-e 's/[ \t]*$//g'    				\
	| grep -v -e FMT_START_SECTION -e FMT_END_SECTION	\
	> doc/autogen/gen.csv

