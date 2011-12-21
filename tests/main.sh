#!/bin/bash
# This is a terribly rough little test harness for some basic functionality in
# notmuchfs.
#
# This file is part of notmuchfs
#
# Notmuchfs is free software, released under the GNU General Public
# License version 3 (or later).
#
# Copyright © 2012 Tim Stoakes
################################################################################

. "include" || exit 1

set -x

trap "cleanup" SIGINT SIGTERM EXIT

function cleanup {
  fusermount -u "$TEST_ROOT/mount"
  rm -Rf "$TEST_ROOT"
}

mkdir -p "$TEST_ROOT"
mkdir -p "$TEST_ROOT/backing"
mkdir -p "$TEST_ROOT/mount"

"$NOTMUCHFS" "$TEST_ROOT/mount" \
  -o backing_dir="$TEST_ROOT/backing" \
  -o mail_dir=~/.maildir/ || die "mount notmuchfs"


ls -al "$TEST_ROOT" >/dev/null || die "list empty root"

QUERY="tag:work and from:admin"

mkdir "$TEST_ROOT/mount/$QUERY" || die "mkdir"
test -d "$TEST_ROOT/mount/$QUERY" || die "dir exists 1"
test -d "$TEST_ROOT/mount/$QUERY/cur" || die "dir exists 2"
test -d "$TEST_ROOT/mount/$QUERY/new" || die "dir exists 3"
test -d "$TEST_ROOT/mount/$QUERY/tmp" || die "dir exists 4"

# Read all the message IDs of the query results, both from notmuchfs and
# directly from notmuch. Compare them, they should be the same.
#
# Use message IDs here instead of files to avoid being tripped up by multiple
# files with the same message ID.
cat "$TEST_ROOT/mount/$QUERY/cur/"* | formail -d -xMessage-id: -s | tr -d "<>" | sort > out1
notmuch search --output=messages "$QUERY" | sed s/id:/\ / | sort > out2
wc -l out1
wc -l out2
diff out1 out2 || die "diff"
rm -f out1 out2


SAVEIFS=$IFS
IFS=$(echo -en "\n\b")
for FILE in `ls -1 "$TEST_ROOT/mount/$QUERY/cur/"*`; do
  ID=`cat "$FILE" | formail -d -xMessage-id: -s | tr -d "<> "`
  # Check that notmuch tags match X-Label: tags.
  TAGS=`notmuch search --output=tags "id:$ID" | tr "\\n" "," |sed s/,\$//`
  XLABEL=`head -n 1 "$FILE" | sed s/X-Label:\ // | sed -e "s/\s\+$//" | tr -d "\\r\\n"`
  [ "$TAGS" == "$XLABEL" ] || die "tags don't match X-Label: \"$TAGS\" vs. \"$XLABEL\"";

  # Check that the rest of the message matches the original ie. notmuchfs
  # didn't alter it.
  tail -n +2 "$FILE" > out1
  notmuch show --format=raw "id:$ID" > out2
  wc -l out1
  wc -l out2
  diff out1 out2 || die "non-tag file content does not match"
  rm -f out1 out2
done
IFS=$SAVEIFS

rmdir "$TEST_ROOT/mount/$QUERY" || die "rmdir"

echo "Success!"
exit 0
