# - Try to find Msgfmt
# Once done, this will define
#
#  Msgfmt_FOUND - system has Gettext
#  Msgfmt_BIN - path to binary file

find_program(Msgfmt_BIN msgfmt)

if(Msgfmt_BIN)
	set(Msgfmt_FOUND 1)
else()
	set(Msgfmt_FOUND 0)
endif(Msgfmt_BIN)