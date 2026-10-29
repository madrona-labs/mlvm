#!/bin/sh

# add-plist-value
#
# This script takes 3 arguments: 
# - key string
# - value string
# - file name
# 
KEY_STRING="$1"
VALUE_STRING="$2"
FILE_NAME="$3"

DICT_END="<\/dict>"
LINE1="<key>"$KEY_STRING"</key>"
LINE2="\t<string>"$VALUE_STRING"</string>"

if [ ! -f "$FILE_NAME" ]; then
	echo "add-plist-value: $FILE_NAME not found" >&2
	exit 1
fi

if
! grep -q "$KEY_STRING" "$FILE_NAME"
then
	perl -i -pe 's/'$DICT_END'/\t<key>'$KEY_STRING'<\/key>\n\t<string>'"$VALUE_STRING"'<\/string>\n'$DICT_END'/;'  "$FILE_NAME"
fi