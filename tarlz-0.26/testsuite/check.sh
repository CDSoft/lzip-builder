#! /bin/sh
# check script for Tarlz - Archiver with multimember lzip compression
# Copyright (C) 2013-2024 Antonio Diaz Diaz.
#
# This script is free software: you have unlimited permission
# to copy, distribute, and modify it.

LC_ALL=C
export LC_ALL
objdir=`pwd`
testdir=`cd "$1" ; pwd`
TARLZ="${objdir}"/tarlz
framework_failure() { echo "failure in testing framework" ; exit 1 ; }

if [ ! -f "${TARLZ}" ] || [ ! -x "${TARLZ}" ] ; then
	echo "${TARLZ}: cannot execute"
	exit 1
fi

[ -e "${TARLZ}" ] 2> /dev/null ||
	{
	echo "$0: a POSIX shell is required to run the tests"
	echo "Try bash -c \"$0 $1 $2\""
	exit 1
	}

if [ -d tmp ] ; then rm -rf tmp ; fi
mkdir tmp
cd "${objdir}"/tmp || framework_failure

in="${testdir}"/test.txt
in_lz="${testdir}"/test.txt.lz
in_tar="${testdir}"/test.txt.tar
in_tar_lz="${testdir}"/test.txt.tar.lz
inbad1="${testdir}"/test_bad1.txt
inbad2="${testdir}"/test_bad2.txt
em_lz="${testdir}"/em.lz
test3="${testdir}"/test3.tar
test3_lz="${testdir}"/test3.tar.lz
test3dir="${testdir}"/test3_dir.tar
test3dir_lz="${testdir}"/test3_dir.tar.lz
test3dot_lz="${testdir}"/test3_dot.tar.lz
t155="${testdir}"/t155.tar
t155_lz="${testdir}"/t155.tar.lz
tlzit1="${testdir}"/tlz_in_tar1.tar
tlzit2="${testdir}"/tlz_in_tar2.tar
bad1="${testdir}"/test3_bad1.tar
bad2="${testdir}"/test3_bad2.tar
bad3="${testdir}"/test3_bad3.tar
bad4="${testdir}"/test3_bad4.tar
bad5="${testdir}"/test3_bad5.tar
bad1_lz="${testdir}"/test3_bad1.tar.lz
bad2_lz="${testdir}"/test3_bad2.tar.lz
bad3_lz="${testdir}"/test3_bad3.tar.lz
bad4_lz="${testdir}"/test3_bad4.tar.lz
bad5_lz="${testdir}"/test3_bad5.tar.lz
bad6_lz="${testdir}"/test3_bad6.tar.lz
eoa="${testdir}"/eoa_blocks.tar
eoa_lz="${testdir}"/eoa_blocks.tar.lz
fail=0
lwarnc=0
test_failed() { fail=1 ; printf " $1" ; [ -z "$2" ] || printf "($2)" ; }
is_compressed() { [ "`dd if="$1" bs=4 count=1 2> /dev/null`" = LZIP ] ; }
is_uncompressed() { [ "`dd if="$1" bs=4 count=1 2> /dev/null`" != LZIP ] ; }
cyg_symlink() { [ ${lwarnc} = 0 ] &&
	printf "\nwarning: your OS follows symbolic links to directories even when tarlz asks it not to\n$1"
	lwarnc=1 ; }

# Description of test files for tarlz:
# test.txt.tar.lz:   1 member (test.txt).
# t155.tar[.lz]:     directory + 3 links + file + EOA, all with 155 char names
# t155_fv?.tar[.lz]: like t155.tar but with 3 kinds of format violations
# t155_fv1.tar[.lz]: extra extended header before EOA blocks
# t155_fv2.tar[.lz]: first extended header followed by global header
# t155_fv3.tar[.lz]: consecutive extended headers in last member
# t155_fv[456].tar.lz: like t155_fv[123].tar.lz but violation alone in member
# tar_in_tlz1.tar.lz: 2 members (test.txt.tar test3.tar) 3 lzip members
# tar_in_tlz2.tar.lz: 2 members (test.txt.tar test3.tar) 5 lzip members
# ts_in_link.tar.lz: 4 symbolic links (link[1-4]) to / /dir/ dir/ dir(107/)
# test_bad1.txt.tar.lz: truncated at offset 6000 (of 7459)
# test_bad2.txt.tar.lz: byte at offset 6000 changed from 0x53 to 0x43
# test3.tar[.lz]:    3 members (foo bar baz) + 2 zeroed 512-byte blocks
# test3_dir.tar[.lz] like test3.tar but members /dir/foo /dir/bar /dir/baz
# test3_dot.tar.lz:  3 times 3 members ./foo ././bar ./././baz
#                    the 3 central members with filename in extended header
# test3_bad1.tar:    byte at offset  259 changed from 't' to '0' (magic)
# test3_bad2.tar:    byte at offset 1283 changed from 't' to '0' (magic)
# test3_bad3.tar:    byte at offset 2559 changed from 0x00 to 0x20 (padding)
# test3_bad4.tar:    byte at offset 1283 changed from 't' to '0' (magic)
#                    byte at offset 2307 changed from 't' to '0' (magic)
# test3_bad5.tar:    510 zeros + "LZ" prepended to test3.tar (bogus lz header)
# test3_bad1.tar.lz: byte at offset    2 changed from 'I' to 'i' (magic)
# test3_bad2.tar.lz: byte at offset   49 changed from 0x49 to 0x69 (mid stream)
# test3_bad3.tar.lz: byte at offset  176 changed from 0x7D to 0x6D (mid stream)
# test3_bad4.tar.lz: combined damage of test3_bad2.tar.lz and test3_bad3.tar.lz
# test3_bad5.tar.lz: [71-134] --> zeroed (first trailer + second header)
# test3_bad6.tar.lz: 510 zeros prepended to test3.tar.lz (header in two blocks)
# test3_eoa?.tar:    like test3_eoa?.tar.lz but uncompressed
# test3_eoa1.tar.lz: test3.tar.lz without EOA blocks
# test3_eoa2.tar.lz: test3.tar.lz with only one EOA block
# test3_eoa3.tar.lz: test3.tar.lz with one zeroed block between foo and bar
# test3_eoa4.tar.lz: test3.tar.lz ended by extended header without EOA blocks
# test3_eoa5.tar.lz: test3.tar.lz split extended bar member, without EOA blocks
# test3_gh?.tar:     test3.tar with global header at each position
# test3_gh?.tar.lz:  test3.tar.lz with global before bar split in 4 ways
# test3_gh5.tar.lz:  test3.tar.lz with global in lzip member before foo
# test3_gh6.tar.lz:  test3.tar.lz with global before foo in same member
# test3_nn.tar[.lz]: test3.tar[.lz] with zeroed name (no name) in bar member
# test3_sm?.tar.lz:  test3.tar.lz with extended bar member split in 4 ways
# tlz_in_tar1.tar:   1 member (test3.tar.lz) first magic damaged
# tlz_in_tar2.tar:   2 members (foo test3.tar.lz) first magic damaged
# ug32chars.tar.lz:  1 member (foo) with 32-character owner and group names
# ug32767.tar.lz:    1 member (foo) with numerical-only owner and group

# Note that multi-threaded --list succeeds with test_bad2.txt.tar.lz and
# test3_bad3.tar.lz because their headers are intact.

"${TARLZ}" --check-lib				# just print warning
[ $? != 2 ] || test_failed $LINENO		# unless bad lzlib.h

printf "testing tarlz-%s..." "$2"

"${TARLZ}" -q -tf "${in}"
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -q -tf "${in_lz}"
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -q -tf "${in_tar_lz}" -f "${in_tar_lz}"
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -tf nx_file
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -tf 2> /dev/null
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -cf out.tar.lz
[ $? = 1 ] || test_failed $LINENO
[ ! -e out.tar.lz ] || test_failed $LINENO
"${TARLZ}" -q -cf out.tar
[ $? = 1 ] || test_failed $LINENO
[ ! -e out.tar ] || test_failed $LINENO
"${TARLZ}" -rf out.tar.lz || test_failed $LINENO
[ ! -e out.tar.lz ] || test_failed $LINENO
"${TARLZ}" -rf out.tar || test_failed $LINENO
[ ! -e out.tar ] || test_failed $LINENO
"${TARLZ}" -r || test_failed $LINENO
"${TARLZ}" -q -rf out.tar.lz "${in}"
[ $? = 1 ] || test_failed $LINENO
[ ! -e out.tar.lz ] || test_failed $LINENO
"${TARLZ}" -q -rf out.tar "${in}"
[ $? = 1 ] || test_failed $LINENO
[ ! -e out.tar ] || test_failed $LINENO
"${TARLZ}" -q -c "${in}" nx_file > /dev/null
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -c -C nx_dir "${in}"
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -x -C nx_dir "${test3_lz}"
[ $? = 1 ] || test_failed $LINENO
touch empty.tar.lz empty.tlz || framework_failure	# list an empty lz file
"${TARLZ}" -q -tf empty.tar.lz
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -q -tf empty.tlz
[ $? = 2 ] || test_failed $LINENO
rm -f empty.tar.lz empty.tlz || framework_failure
touch empty.tar || framework_failure		# compress an empty archive
"${TARLZ}" -q -z empty.tar
[ $? = 2 ] || test_failed $LINENO
[ ! -e empty.tar.lz ] || test_failed $LINENO
rm -f empty.tar empty.tar.lz || framework_failure
"${TARLZ}" -q -cd				# test mixed operations
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -cr
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -ct
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -cx
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -tx
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -ctx
[ $? = 1 ] || test_failed $LINENO
for i in A c d r t x -delete ; do	# test -o with operations other than -z
	"${TARLZ}" -q -$i -o -
	[ $? = 1 ] || test_failed $LINENO $i
done
"${TARLZ}" -q -z -f -
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -z .
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -z -o - --uncompressed "${test3}" > /dev/null 2>&1
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -q -tf "${in_tar_lz}" ""		# empty non-option argument
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" --help > /dev/null || test_failed $LINENO
"${TARLZ}" -V > /dev/null || test_failed $LINENO
"${TARLZ}" --bad_option -tf "${test3_lz}" 2> /dev/null
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" -tf 2> /dev/null
[ $? = 1 ] || test_failed $LINENO
bad_dates='@-9223372036854775809 @9223372036854775808
           -2147481749-01-01T00:00:00 2147483648-01-01T00:00:00
           2017-10-01T 2017-10 ./nx_file'
for i in ${bad_dates} ; do
  "${TARLZ}" -c --mtime="$i" "${in}" > /dev/null 2>&1
  [ $? = 1 ] || test_failed $LINENO "$i"
done
"${TARLZ}" --owner=invalid_owner_name -tf "${test3_lz}" 2> /dev/null
[ $? = 1 ] || test_failed $LINENO
"${TARLZ}" --group=invalid_group_name -tf "${test3_lz}" 2> /dev/null
[ $? = 1 ] || test_failed $LINENO

printf "\ntesting --list and --extract..."

# test --list and --extract
"${TARLZ}" -tf "${eoa_lz}" --missing-crc || test_failed $LINENO
"${TARLZ}" -xf "${eoa_lz}" --missing-crc || test_failed $LINENO
"${TARLZ}" -C nx_dir -tf "${in_tar}" > /dev/null || test_failed $LINENO
"${TARLZ}" -xf "${in_tar}" --missing-crc || test_failed $LINENO
cmp "${in}" test.txt || test_failed $LINENO
rm -f test.txt || framework_failure
"${TARLZ}" -tf "${in_tar_lz}" --missing-crc > /dev/null || test_failed $LINENO
for i in 0 2 6 ; do
  "${TARLZ}" -n$i -xf "${in_tar_lz}" --missing-crc || test_failed $LINENO $i
  cmp "${in}" test.txt || test_failed $LINENO $i
  rm -f test.txt || framework_failure
done

# test3 reference files for -t and -tv (list3, vlist3)
"${TARLZ}" -tf "${test3}" > list3 || test_failed $LINENO
"${TARLZ}" -tvf "${test3}" > vlist3 || test_failed $LINENO
for i in 0 2 6 ; do
	"${TARLZ}" -n$i -tf "${test3_lz}" > out || test_failed $LINENO $i
	diff -u list3 out || test_failed $LINENO $i
	"${TARLZ}" -n$i -tvf "${test3_lz}" > out || test_failed $LINENO $i
	diff -u vlist3 out || test_failed $LINENO $i
done
rm -f out || framework_failure

# test3 reference files for cmp
cp "${testdir}"/rfoo cfoo || framework_failure
cp "${testdir}"/rbar cbar || framework_failure
cp "${testdir}"/rbaz cbaz || framework_failure

# test --list and --extract test3
rm -f foo bar baz || framework_failure
"${TARLZ}" -xf "${test3}" --missing-crc || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
# time and mode comparison always fails on OS/2
if "${TARLZ}" -df "${test3}" --ignore-ids ; then d_works=yes
else printf "warning: some '--diff' tests will be skipped.\n"
fi
rm -f foo bar baz || framework_failure
for i in 0 2 6 ; do
  "${TARLZ}" -n$i -xf "${test3_lz}" --missing-crc || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -n$i -tvf "${test3_lz}" ./foo ./bar ./baz > out 2> /dev/null ||
    test_failed $LINENO $i
  diff -u vlist3 out || test_failed $LINENO $i
  rm -f out || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3_lz}" ./foo ./bar ./baz || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -n$i -xf "${test3_lz}" foo/ bar// baz/// || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dot_lz}" --missing-crc || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -q -n$i -tf "${test3dot_lz}" foo bar baz || test_failed $LINENO $i
  "${TARLZ}" -q -n$i -xf "${test3dot_lz}" foo bar baz || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
done

# test -C in --diff and --extract
for i in "${test3}" "${test3_lz}" ; do
  mkdir dir1 dir2 dir3 || framework_failure
  "${TARLZ}" -q -xf "$i" -C dir1 foo -C ../dir2 bar -C ../dir3 baz ||
    test_failed $LINENO "$i"
  cmp cfoo dir1/foo || test_failed $LINENO "$i"
  cmp cbar dir2/bar || test_failed $LINENO "$i"
  cmp cbaz dir3/baz || test_failed $LINENO "$i"
  if [ "${d_works}" = yes ] ; then
    "${TARLZ}" -df "$i" -C dir1 foo -C ../dir2 --ignore-ids bar \
      -C ../dir3 baz || test_failed $LINENO "$i"
    "${TARLZ}" -df "$i" -C dir3 baz -C ../dir2 bar -C ../dir1 foo \
      --ignore-ids || test_failed $LINENO "$i"
  fi
  rm -rf dir1 dir2 dir3 || framework_failure
done
for i in "${test3dir}" "${test3dir_lz}" ; do
  mkdir dir1 dir2 dir3 || framework_failure
  "${TARLZ}" -q -xf "$i" -C dir2 dir/bar -C ../dir1 dir/foo \
    -C ../dir3 dir/baz || test_failed $LINENO "$i"
  cmp cfoo dir1/dir/foo || test_failed $LINENO "$i"
  cmp cbar dir2/dir/bar || test_failed $LINENO "$i"
  cmp cbaz dir3/dir/baz || test_failed $LINENO "$i"
  if [ "${d_works}" = yes ] ; then
    "${TARLZ}" -q -df "$i" --ignore-ids -C dir1 dir/foo -C ../dir2 dir/bar \
      -C ../dir3 dir/baz || test_failed $LINENO "$i"
    "${TARLZ}" -q -df "${test3}" -C dir1/dir foo -C ../../dir2/dir bar \
      --ignore-ids -C ../../dir3/dir baz || test_failed $LINENO "$i"
  fi
  rm -rf dir1 dir2 dir3 || framework_failure
done

for i in "${test3dir}" "${test3dir_lz}" ; do
	"${TARLZ}" -q -tf "$i" --missing-crc || test_failed $LINENO "$i"
	"${TARLZ}" -q -xf "$i" --missing-crc || test_failed $LINENO "$i"
	cmp cfoo dir/foo || test_failed $LINENO "$i"
	cmp cbar dir/bar || test_failed $LINENO "$i"
	cmp cbaz dir/baz || test_failed $LINENO "$i"
	rm -rf dir || framework_failure
	"${TARLZ}" -q -tf "$i" dir || test_failed $LINENO "$i"
	"${TARLZ}" -q -xf "$i" dir || test_failed $LINENO "$i"
	cmp cfoo dir/foo || test_failed $LINENO "$i"
	cmp cbar dir/bar || test_failed $LINENO "$i"
	cmp cbaz dir/baz || test_failed $LINENO "$i"
	rm -rf dir || framework_failure
	"${TARLZ}" -q -tf "$i" dir/foo dir/baz || test_failed $LINENO "$i"
	"${TARLZ}" -q -xf "$i" dir/foo dir/baz || test_failed $LINENO "$i"
	cmp cfoo dir/foo || test_failed $LINENO "$i"
	[ ! -e dir/bar ] || test_failed $LINENO "$i"
	cmp cbaz dir/baz || test_failed $LINENO "$i"
	rm -rf dir || framework_failure
done

# test --extract --exclude
"${TARLZ}" -xf "${test3}" --exclude='f*o' --exclude=baz || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f foo bar baz || framework_failure
for i in 0 2 6 ; do
  "${TARLZ}" -n$i -xf "${test3_lz}" --exclude=bar || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  [ ! -e bar ] || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dir_lz}" --exclude='?ar' || test_failed $LINENO $i
  cmp cfoo dir/foo || test_failed $LINENO $i
  [ ! -e dir/bar ] || test_failed $LINENO $i
  cmp cbaz dir/baz || test_failed $LINENO $i
  rm -rf dir || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dir_lz}" --exclude=dir/bar || test_failed $LINENO $i
  cmp cfoo dir/foo || test_failed $LINENO $i
  [ ! -e dir/bar ] || test_failed $LINENO $i
  cmp cbaz dir/baz || test_failed $LINENO $i
  rm -rf dir || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dir_lz}" --exclude=dir || test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
  rm -rf dir || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dir_lz}" --exclude='dir/*' || test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
  rm -rf dir || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dir_lz}" --exclude='[bf][ao][orz]' ||
    test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
  rm -rf dir || framework_failure
  "${TARLZ}" -q -n$i -xf "${test3dir_lz}" --exclude='*o' dir/foo ||
    test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
  rm -rf dir || framework_failure
done

# test --list and --extract EOA
"${TARLZ}" -tvf "${testdir}"/test3_eoa1.tar > out 2> /dev/null
[ $? = 2 ] || test_failed $LINENO
diff -u vlist3 out || test_failed $LINENO
"${TARLZ}" -tvf "${testdir}"/test3_eoa2.tar > out || test_failed $LINENO
diff -u vlist3 out || test_failed $LINENO
"${TARLZ}" -q -tf "${testdir}"/test3_eoa3.tar || test_failed $LINENO
"${TARLZ}" -tvf "${testdir}"/test3_eoa4.tar > out 2> /dev/null
[ $? = 2 ] || test_failed $LINENO
diff -u vlist3 out || test_failed $LINENO
for i in 0 2 6 ; do
	"${TARLZ}" -n$i -tvf "${testdir}"/test3_eoa1.tar.lz > out 2> /dev/null
	[ $? = 2 ] || test_failed $LINENO $i
	diff -u vlist3 out || test_failed $LINENO $i
	"${TARLZ}" -n$i -tvf "${testdir}"/test3_eoa2.tar.lz > out ||
		test_failed $LINENO $i
	diff -u vlist3 out || test_failed $LINENO $i
	"${TARLZ}" -q -n$i -tf "${testdir}"/test3_eoa3.tar.lz ||
		test_failed $LINENO $i
	"${TARLZ}" -n$i -tvf "${testdir}"/test3_eoa4.tar.lz > out 2> /dev/null
	[ $? = 2 ] || test_failed $LINENO $i
	diff -u vlist3 out || test_failed $LINENO $i
	"${TARLZ}" -n$i -tvf "${testdir}"/test3_eoa5.tar.lz > out 2> /dev/null
	[ $? = 2 ] || test_failed $LINENO $i
	diff -u vlist3 out || test_failed $LINENO $i
done
rm -f out || framework_failure
#
"${TARLZ}" -q -xf "${testdir}"/test3_eoa1.tar
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -xf "${testdir}"/test3_eoa2.tar || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -xf "${testdir}"/test3_eoa3.tar || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf "${testdir}"/test3_eoa4.tar
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
#
for i in 0 2 6 ; do
  "${TARLZ}" -q -n$i -xf "${testdir}"/test3_eoa1.tar.lz
  [ $? = 2 ] || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -n$i -xf "${testdir}"/test3_eoa2.tar.lz || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -q -n$i -xf "${testdir}"/test3_eoa4.tar.lz
  [ $? = 2 ] || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -q -n$i -xf "${testdir}"/test3_eoa5.tar.lz
  [ $? = 2 ] || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz || framework_failure
done
"${TARLZ}" -n0 -xf "${testdir}"/test3_eoa3.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f foo bar baz || framework_failure

# test --list and --extract tar in tar.lz
for i in "${testdir}"/tar_in_tlz1.tar.lz "${testdir}"/tar_in_tlz2.tar.lz ; do
	for j in 0 2 6 ; do
		"${TARLZ}" -tf "$i" -n$j > out$j ||
			test_failed $LINENO "$i $j"
		"${TARLZ}" -tvf "$i" -n$j > outv$j ||
			test_failed $LINENO "$i $j"
	done
	diff -u out0 out2 || test_failed $LINENO "$i"
	diff -u out0 out6 || test_failed $LINENO "$i"
	diff -u out2 out6 || test_failed $LINENO "$i"
	diff -u outv0 outv2 || test_failed $LINENO "$i"
	diff -u outv0 outv6 || test_failed $LINENO "$i"
	diff -u outv2 outv6 || test_failed $LINENO "$i"
	rm -f out0 out2 out6 outv0 outv2 outv6 || framework_failure
	for j in 0 2 6 ; do
		"${TARLZ}" -xf "$i" -n$j || test_failed $LINENO "$i $j"
		cmp "${in_tar}" test.txt.tar || test_failed $LINENO "$i $j"
		cmp "${test3}" test3.tar || test_failed $LINENO "$i $j"
		rm -f test.txt.tar test3.tar || framework_failure
	done
done

# test --list and --extract with global headers uncompressed
for i in gh1 gh2 gh3 gh4 ; do
  "${TARLZ}" -tf "${testdir}"/test3_${i}.tar > out || test_failed $LINENO $i
  diff -u list3 out || test_failed $LINENO $i
  "${TARLZ}" -tvf "${testdir}"/test3_${i}.tar > out || test_failed $LINENO $i
  diff -u vlist3 out || test_failed $LINENO $i
  "${TARLZ}" -xf "${testdir}"/test3_${i}.tar || test_failed $LINENO $i
  cmp cfoo foo || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f foo bar baz out || framework_failure
done

# test --list and --extract with empty lzip member
cat "${em_lz}" "${test3_lz}" > test3_em.tar.lz || framework_failure
"${TARLZ}" -q -tf test3_em.tar.lz > out
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -tvf - < test3_em.tar.lz > out || test_failed $LINENO
diff -u vlist3 out || test_failed $LINENO
"${TARLZ}" -q -xf test3_em.tar.lz
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
"${TARLZ}" -xf - < test3_em.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure

# test --list and --extract with global headers and extended tar members
# split among lzip members
for i in gh1 gh2 gh3 gh4 gh5 gh6 sm1 sm2 sm3 sm4 ; do
	for j in 0 2 6 ; do
		"${TARLZ}" -n$j -tf "${testdir}"/test3_${i}.tar.lz > out ||
			test_failed $LINENO "$i $j"
		diff -u list3 out || test_failed $LINENO "$i $j"
		"${TARLZ}" -n$j -tvf "${testdir}"/test3_${i}.tar.lz > out ||
			test_failed $LINENO "$i $j"
		diff -u vlist3 out || test_failed $LINENO "$i $j"
	done
	rm -f out || framework_failure
	for j in 0 2 6 ; do
		"${TARLZ}" -n$j -xf "${testdir}"/test3_${i}.tar.lz ||
			test_failed $LINENO "$i $j"
		cmp cfoo foo || test_failed $LINENO "$i $j"
		cmp cbar bar || test_failed $LINENO "$i $j"
		cmp cbaz baz || test_failed $LINENO "$i $j"
		rm -f foo bar baz || framework_failure
	done
done
rm -f list3 vlist3 || framework_failure

printf "\ntesting --concatenate..."

# test --concatenate compressed
cp "${in}" out.tar.lz || framework_failure		# invalid tar.lz
"${TARLZ}" -Aqf out.tar.lz "${test3_lz}"
[ $? = 2 ] || test_failed $LINENO
cp "${in_tar_lz}" out.tar.lz || framework_failure
"${TARLZ}" -q --un -Af out.tar.lz "${test3_lz}"		# contradictory ext
[ $? = 1 ] || test_failed $LINENO
cmp "${in_tar_lz}" out.tar.lz || test_failed $LINENO
cp "${in_tar_lz}" out.tar.lz || framework_failure
"${TARLZ}" -Af out.tar.lz "${test3_lz}" || test_failed $LINENO
"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
cmp "${in}" test.txt || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f test.txt foo bar baz || framework_failure
touch aout.tar.lz || framework_failure		# concatenate to empty file
"${TARLZ}" -Aqf aout.tar.lz "${in_tar}"
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -Af aout.tar.lz "${in_tar_lz}" "${test3_lz}" || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -Af aout.tar.lz || test_failed $LINENO	# concatenate nothing
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -Aqf aout.tar.lz aout.tar.lz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -Aq "${in_tar_lz}" "${test3}" > aout.tar.lz		# to stdout
[ $? = 2 ] || test_failed $LINENO
cmp "${in_tar_lz}" aout.tar.lz || test_failed $LINENO
"${TARLZ}" -A "${in_tar_lz}" "${test3_lz}" > aout.tar.lz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
cp "${eoa_lz}" aout.tar.lz || framework_failure
"${TARLZ}" -Aqf aout.tar.lz "${in_tar}"		# concatenate to empty archive
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -Af aout.tar.lz "${in_tar_lz}" "${test3_lz}" || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
cp "${in_tar_lz}" aout.tar.lz || framework_failure
"${TARLZ}" -Aqf aout.tar.lz "${test3_lz}" "${test3}"
[ $? = 2 ] || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f aout.tar.lz || framework_failure
touch aout.tar.lz || framework_failure			# --exclude
"${TARLZ}" -Af aout.tar.lz "${in_tar_lz}" "${test3_lz}" --exclude 'test3*' ||
	test_failed $LINENO
"${TARLZ}" -Af aout.tar.lz "${in_tar_lz}" "${test3_lz}" --exclude '*txt*' ||
	test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f out.tar.lz aout.tar.lz || framework_failure

# test --concatenate uncompressed
cp "${in}" out.tar || framework_failure		# invalid tar
"${TARLZ}" -Aqf out.tar "${test3}"
[ $? = 2 ] || test_failed $LINENO
cp "${in_tar}" out.tar || framework_failure
"${TARLZ}" -q -0 -Af out.tar "${test3}"			# contradictory ext
[ $? = 1 ] || test_failed $LINENO
cmp "${in_tar}" out.tar || test_failed $LINENO
cp "${in_tar}" out.tar || framework_failure
"${TARLZ}" -Af out.tar "${test3}" || test_failed $LINENO
"${TARLZ}" -xf out.tar || test_failed $LINENO
cmp "${in}" test.txt || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f test.txt foo bar baz || framework_failure
touch aout.tar || framework_failure		# concatenate to empty file
"${TARLZ}" -Aqf aout.tar "${in_tar_lz}"
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -Af aout.tar "${in_tar}" "${test3}" || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -Af aout.tar || test_failed $LINENO	# concatenate nothing
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -Aqf aout.tar aout.tar || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -Aq "${in_tar}" "${test3_lz}" > aout.tar		# to stdout
[ $? = 2 ] || test_failed $LINENO
cmp "${in_tar}" aout.tar || test_failed $LINENO
"${TARLZ}" -A "${in_tar}" "${test3}" > aout.tar || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
cp "${eoa}" aout.tar || framework_failure	# concatenate to empty archive
"${TARLZ}" -Aqf aout.tar "${in_tar_lz}"
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -Af aout.tar "${in_tar}" "${test3}" || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
cp "${in_tar}" aout.tar || framework_failure
"${TARLZ}" -Aqf aout.tar "${test3}" "${test3_lz}"
[ $? = 2 ] || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
rm -f aout.tar || framework_failure
touch aout.tar || framework_failure			# --exclude
"${TARLZ}" -Af aout.tar "${test3}" "${in_tar}" --exclude 'test3*' ||
	test_failed $LINENO
"${TARLZ}" -Af aout.tar "${test3}" "${in_tar}" --exclude '*txt*' ||
	test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
rm -f out.tar aout.tar || framework_failure

printf "\ntesting --create..."

# test --create
cp "${in}" test.txt || framework_failure
"${TARLZ}" --warn-newer -0 -cf out.tar.lz test.txt || test_failed $LINENO
is_compressed out.tar.lz || test_failed $LINENO
rm -f test.txt || framework_failure
"${TARLZ}" -xf out.tar.lz --missing-crc || test_failed $LINENO
cmp "${in}" test.txt || test_failed $LINENO
cp "${in}" test.txt || framework_failure
"${TARLZ}" --warn-newer --un -cf out.tar test.txt || test_failed $LINENO
is_uncompressed out.tar || test_failed $LINENO
rm -f test.txt || framework_failure
"${TARLZ}" -xf out.tar --missing-crc || test_failed $LINENO
cmp "${in}" test.txt || test_failed $LINENO
rm -f test.txt out.tar out.tar.lz || framework_failure

cp cfoo foo || framework_failure
rm -f bar || framework_failure
cp cbaz baz || framework_failure
"${TARLZ}" -0 -q -cf out.tar.lz foo bar baz
[ $? = 1 ] || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -xf out.tar.lz --missing-crc || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf out.tar.lz bar
[ $? = 1 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f out.tar.lz || framework_failure

cp cfoo foo || framework_failure
cp cbar bar || framework_failure
cp cbaz baz || framework_failure
"${TARLZ}" -0 -cf out.tar.lz foo bar baz --out-slots=1 || test_failed $LINENO
"${TARLZ}" -0 -q -cf aout.tar.lz foo bar aout.tar.lz baz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO	# test reproducible
rm -f aout.tar.lz || framework_failure
#
"${TARLZ}" -0 -cf aout.tar.lz foo bar baz -C / || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f aout.tar.lz || framework_failure
"${TARLZ}" -0 -C / -cf aout.tar.lz -C "${objdir}"/tmp foo bar baz ||
	test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f aout.tar.lz || framework_failure
"${TARLZ}" --asolid -0 -cf aout.tar.lz foo bar baz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f aout.tar.lz || framework_failure
"${TARLZ}" -0 -q -cf aout.tar.lz foo/ ./bar ./baz/ || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f aout.tar.lz || framework_failure
mkdir dir1 || framework_failure
"${TARLZ}" -C dir1 -xf out.tar.lz || test_failed $LINENO
cmp cfoo dir1/foo || test_failed $LINENO
cmp cbar dir1/bar || test_failed $LINENO
cmp cbaz dir1/baz || test_failed $LINENO
rm -f aout.tar.lz foo bar baz || framework_failure
"${TARLZ}" -C dir1 -0 -cf aout.tar.lz foo bar baz || test_failed $LINENO
"${TARLZ}" -xf aout.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f aout.tar.lz foo bar baz || framework_failure
"${TARLZ}" -C dir1 -0 -c foo bar baz | "${TARLZ}" -x || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f dir1/foo dir1/bar dir1/baz || framework_failure
"${TARLZ}" -0 -c foo bar baz | "${TARLZ}" -C dir1 -x || test_failed $LINENO
cmp cfoo dir1/foo || test_failed $LINENO
cmp cbar dir1/bar || test_failed $LINENO
cmp cbaz dir1/baz || test_failed $LINENO
rm -f dir1/foo dir1/bar dir1/baz || framework_failure
"${TARLZ}" -0 -c foo bar baz | "${TARLZ}" -x -C dir1 foo bar baz ||
	test_failed $LINENO
cmp cfoo dir1/foo || test_failed $LINENO
cmp cbar dir1/bar || test_failed $LINENO
cmp cbaz dir1/baz || test_failed $LINENO
rm -f foo dir1/bar baz || framework_failure
"${TARLZ}" -0 -cf aout.tar.lz -C dir1 foo -C .. bar -C dir1 baz ||
	test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -0 -cf aout.tar.lz dir1/foo dir1/baz || test_failed $LINENO
rm -rf dir1 bar || framework_failure
"${TARLZ}" -xf aout.tar.lz dir1 || test_failed $LINENO
cmp cfoo dir1/foo || test_failed $LINENO
cmp cbaz dir1/baz || test_failed $LINENO
rm -rf dir1 || framework_failure
rm -f out.tar.lz aout.tar.lz || framework_failure

# test --create --exclude
cp cfoo foo || framework_failure
cp cbar bar || framework_failure
cp cbaz baz || framework_failure
"${TARLZ}" -0 -cf out.tar.lz foo bar baz --exclude 'ba?' || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f out.tar.lz foo bar baz || framework_failure
cp cfoo foo || framework_failure
cp cbar bar || framework_failure
cp cbaz baz || framework_failure
"${TARLZ}" -cf out.tar foo bar baz --exclude 'ba*' || test_failed $LINENO
is_uncompressed out.tar || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -xf out.tar || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f out.tar foo bar baz || framework_failure

# test --create --mtime
dates='@-9223372036854775808 @-9223372036854775807
       -2147481748-12-31T23:59:59 -1970-01-01T00:00:00
       0000-01-01T00:00:00 0000-01-01T00:00:01 0000-01-02T00:00:00
       1697-10-17T11:03:27 1697-10-17T11:03:28 1697-10-17T11:03:29
       1833-11-24T17:31:43 1833-11-24T17:31:44 1833-11-24T17:31:45
       1901-12-13T20:45:51 1901-12-13T20:45:52 1901-12-13T20:45:53
       1901-12-14T20:45:51
       1969-12-31T23:59:58 1969-12-31T23:59:59
       1970-01-01T00:00:00 1970-01-01T00:00:01 @0
       2038-01-18T03:14:07 2038-01-19T03:14:07 2038-01-19T03:14:08
       2106-02-07T06:28:15 2106-02-07T06:28:16
       2242-03-16T12:56:31 2242-03-16T12:56:32 @8589934591 @8589934592
       9999-12-31T23:59:58 9999-12-31T23:59:59
       2147483647-12-31T23:59:59 @9223372036854775807'
touch -d 2022-01-05T12:22:13 bar >/dev/null 2>&1 ||
touch -d '2022-01-05 12:22:13' bar >/dev/null 2>&1 ||
touch bar || framework_failure
for i in ${dates} @-8Ei '2017-10-01 09:00:00' '2017-10-1 9:0:0' \
         '2017-10-01 09:00' '2017-10-01 09' 2017-10-01 ./bar ; do
  # Skip a time stamp $i if it's out of range for this platform,
  # or if it uses a notation that this platform does not recognize.
  [ "$i" = "./bar" ] || touch -d "$i" foo >/dev/null 2>&1 || continue
  touch foo || framework_failure
  "${TARLZ}" -cf out.tar --mtime="$i" foo || test_failed $LINENO "$i"
  is_uncompressed out.tar || test_failed $LINENO "$i"
  "${TARLZ}" -q -df out.tar && test_failed $LINENO "$i"
  "${TARLZ}" -xf out.tar || test_failed $LINENO "$i"
  if [ "${d_works}" = yes ] ; then
    "${TARLZ}" -df out.tar --ignore-overflow || test_failed $LINENO "$i"
  fi
done
rm -f out.tar foo bar || framework_failure

mkdir dir || framework_failure
for i in ${dates} ; do touch -d "$i" "dir/f$i" >/dev/null 2>&1 ; done
"${TARLZ}" -cf out.tar dir || test_failed $LINENO
is_uncompressed out.tar || test_failed $LINENO
"${TARLZ}" -df out.tar || test_failed $LINENO
rm -rf out.tar dir || framework_failure

printf "\ntesting --diff..."

"${TARLZ}" -xf "${test3_lz}" || test_failed $LINENO
"${TARLZ}" -cf out.tar foo || test_failed $LINENO
"${TARLZ}" -cf aout.tar foo --anonymous || test_failed $LINENO
is_uncompressed out.tar || test_failed $LINENO
is_uncompressed aout.tar || test_failed $LINENO
if cmp out.tar aout.tar > /dev/null ; then
  printf "\nwarning: '--diff' test can't be run as root.\n"
else
  for i in 0 2 6 ; do
    "${TARLZ}" -n$i -xf "${test3_lz}" || test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${test3_lz}" > out$i
    [ $? = 1 ] || test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${test3_lz}" --ignore-ids || test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${test3_lz}" --exclude '*' || test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${in_tar_lz}" --exclude '*' || test_failed $LINENO $i
    rm -f bar || framework_failure
    "${TARLZ}" -n$i -df "${test3_lz}" --ignore-ids foo baz ||
      test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${test3_lz}" --ignore-metadata foo baz ||
      test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${test3_lz}" --exclude bar --ignore-ids ||
      test_failed $LINENO $i
    rm -f foo baz || framework_failure
    "${TARLZ}" -q -n$i -xf "${test3dir_lz}" || test_failed $LINENO $i
    "${TARLZ}" -q -n$i -df "${test3dir_lz}" --ignore-ids ||
      test_failed $LINENO $i
    "${TARLZ}" -q -n$i -df "${test3dir_lz}" dir --ignore-ids ||
      test_failed $LINENO $i
    "${TARLZ}" -n$i -df "${test3_lz}" --ignore-ids -C dir ||
      test_failed $LINENO $i
    rm -rf dir || framework_failure
  done
  cmp out0 out2 || test_failed $LINENO
  cmp out0 out6 || test_failed $LINENO
  rm -f out0 out2 out6 || framework_failure
fi
rm -f out.tar aout.tar foo bar baz || framework_failure

printf "\ntesting --delete..."

# test --delete
cp "${in}" out.tar || framework_failure		# invalid tar
"${TARLZ}" -q -f out.tar --delete foo
[ $? = 2 ] || test_failed $LINENO
rm -f out.tar || framework_failure
cp "${in}" out.tar.lz || framework_failure		# invalid tar.lz
"${TARLZ}" -q -f out.tar.lz --delete foo
[ $? = 2 ] || test_failed $LINENO
cp "${in_lz}" out.tar.lz || framework_failure		# invalid tar.lz
"${TARLZ}" -q -f out.tar.lz --delete foo
[ $? = 2 ] || test_failed $LINENO
rm -f out.tar.lz || framework_failure

for e in "" .lz ; do
  "${TARLZ}" -A "${in_tar}"$e "${test3}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -f out.tar$e --delete test.txt || test_failed $LINENO $e
  cmp "${test3}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -f out.tar$e --delete || test_failed $LINENO $e	# delete nothing
  cmp "${test3}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -q -f out.tar$e --delete nx_file
  [ $? = 1 ] || test_failed $LINENO $e
  cmp "${test3}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3dir}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -q -f out.tar$e --delete test.txt || test_failed $LINENO $e
  cmp "${test3dir}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3dir}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -q -f out.tar$e --delete dir || test_failed $LINENO $e
  cmp "${in_tar}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3dir}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -q -f out.tar$e --del dir/foo dir/bar dir/baz || test_failed $LINENO $e
  cmp "${in_tar}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3dir}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -q -f out.tar$e --del dir/foo dir/baz || test_failed $LINENO $e
  cmp "${in_tar}"$e out.tar$e > /dev/null && test_failed $LINENO $e
  "${TARLZ}" -q -f out.tar$e --del dir/bar || test_failed $LINENO $e
  cmp "${in_tar}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -f out.tar$e --delete foo bar baz || test_failed $LINENO $e
  cmp "${in_tar}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3}"$e > out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -f out.tar$e --del test.txt foo bar baz || test_failed $LINENO $e
  cmp "${eoa}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3}"$e > out.tar$e || test_failed $LINENO $e
  for i in test.txt foo bar baz ; do
    "${TARLZ}" -f out.tar$e --delete $i || test_failed $LINENO "$e $i"
  done
  cmp "${eoa}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3}"$e > out.tar$e || test_failed $LINENO $e
  for i in baz bar foo test.txt ; do
    "${TARLZ}" -f out.tar$e --delete $i || test_failed $LINENO "$e $i"
  done
  cmp "${eoa}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${test3}"$e > out.tar$e || test_failed $LINENO $e
  for i in foo bar test.txt baz ; do
    "${TARLZ}" -f out.tar$e --delete $i || test_failed $LINENO "$e $i"
  done
  cmp "${eoa}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -A "${in_tar}"$e "${t155}"$e "${test3}"$e > out.tar$e ||
    test_failed $LINENO $e
  "${TARLZ}" -f out.tar$e --del baz foo test.txt bar || test_failed $LINENO $e
  cmp "${t155}"$e out.tar$e || test_failed $LINENO $e
  "${TARLZ}" -f out.tar$e --delete link || test_failed $LINENO $e
  "${TARLZ}" -q -tf out.tar$e || test_failed $LINENO $e
  cmp "${t155}"$e out.tar$e > /dev/null && test_failed $LINENO $e
  rm -f out.tar$e || framework_failure
done

# test --delete individual member after collective member
cp cfoo foo || framework_failure
cp cbar bar || framework_failure
cp cbaz baz || framework_failure
cp "${in}" test.txt || framework_failure
"${TARLZ}" -0 -cf out.tar.lz foo bar baz --asolid || test_failed $LINENO
"${TARLZ}" -0 -rf out.tar.lz test.txt || test_failed $LINENO
rm -f foo bar baz test.txt || framework_failure
for i in foo bar baz ; do
	"${TARLZ}" -q -f out.tar.lz --delete $i
	[ $? = 2 ] || test_failed $LINENO $i
done
"${TARLZ}" -f out.tar.lz --delete test.txt || test_failed $LINENO
"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
[ ! -e test.txt ] || test_failed $LINENO
rm -f out.tar.lz foo bar baz test.txt || framework_failure

# test --delete with empty lzip member, global header
"${TARLZ}" -q -f test3_em.tar.lz --delete foo
[ $? = 2 ] || test_failed $LINENO
rm -f test3_em.tar.lz || framework_failure
cp "${testdir}"/test3_gh5.tar.lz out.tar.lz || framework_failure
for i in foo bar baz ; do
	"${TARLZ}" -f out.tar.lz --delete $i || test_failed $LINENO $i
done
rm -f out.tar.lz || framework_failure
for i in 1 2 3 4 ; do
	cp "${testdir}"/test3_gh${i}.tar out.tar || framework_failure
	for j in foo bar baz ; do
		"${TARLZ}" -f out.tar --delete $j || test_failed $LINENO "$i $j"
	done
	rm -f out.tar || framework_failure
done

printf "\ntesting --dereference..."

# test --dereference
touch dummy_file || framework_failure
if ln dummy_file dummy_link 2> /dev/null &&
   ln -s dummy_file dummy_slink 2> /dev/null ; then
	ln_works=yes
else
	printf "\nwarning: skipping link test: 'ln' does not work on your system.\n"
fi
rm -f dummy_slink dummy_link dummy_file || framework_failure
#
if [ "${ln_works}" = yes ] ; then
	mkdir dir || framework_failure
	cp cfoo dir/foo || framework_failure
	cp cbar dir/bar || framework_failure
	cp cbaz dir/baz || framework_failure
	ln -s dir dir_link || framework_failure
	"${TARLZ}" -0 -c dir_link > out1 || test_failed $LINENO
	is_compressed out1 || test_failed $LINENO
	"${TARLZ}" --un -c dir_link > out2 || test_failed $LINENO
	is_uncompressed out2 || test_failed $LINENO
	"${TARLZ}" -0 -n0 -c dir_link > out3 || test_failed $LINENO
	"${TARLZ}" -0 -h -c dir_link > hout1 || test_failed $LINENO
	"${TARLZ}" --un -h -c dir_link > hout2 || test_failed $LINENO
	"${TARLZ}" -0 -n0 -h -c dir_link > hout3 || test_failed $LINENO
	rm -rf dir dir_link || framework_failure
	for i in 1 2 3 ; do
		"${TARLZ}" -xf out$i --exclude='dir_link/*' dir_link ||
			test_failed $LINENO $i	# Cygwin stores dir_link/*
		[ -h dir_link ] || test_failed $LINENO $i
		"${TARLZ}" -q -tf out$i dir_link/foo && cyg_symlink $LINENO $i
		"${TARLZ}" -q -tf out$i dir_link/bar && cyg_symlink $LINENO $i
		"${TARLZ}" -q -tf out$i dir_link/baz && cyg_symlink $LINENO $i
		rm -rf dir_link out$i || framework_failure
		"${TARLZ}" -xf hout$i || test_failed $LINENO $i
		[ -d dir_link ] || test_failed $LINENO $i
		cmp cfoo dir_link/foo || test_failed $LINENO $i
		cmp cbar dir_link/bar || test_failed $LINENO $i
		cmp cbaz dir_link/baz || test_failed $LINENO $i
		rm -rf dir_link hout$i || framework_failure
	done
fi

printf "\ntesting --append..."

# test --append compressed
cp cfoo foo || framework_failure
cp cbar bar || framework_failure
cp cbaz baz || framework_failure
"${TARLZ}" -0 -cf out.tar.lz foo bar baz --out-slots=1024 || test_failed $LINENO
"${TARLZ}" -0 -cf nout.tar.lz foo bar baz --no-solid || test_failed $LINENO
"${TARLZ}" -0 -cf aout.tar.lz foo || test_failed $LINENO
"${TARLZ}" -0 -rf aout.tar.lz bar baz --no-solid || test_failed $LINENO
cmp nout.tar.lz aout.tar.lz || test_failed $LINENO
rm -f nout.tar.lz aout.tar.lz || framework_failure
touch aout.tar.lz || framework_failure		# append to empty file
"${TARLZ}" -0 -rf aout.tar.lz foo bar baz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -0 -rf aout.tar.lz || test_failed $LINENO	# append nothing
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -0 -rf aout.tar.lz -C nx_dir || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -0 -q -rf aout.tar.lz nx_file
[ $? = 1 ] || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -0 -q -rf aout.tar.lz aout.tar.lz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" -0 -r foo bar baz > aout.tar.lz || test_failed $LINENO  # to stdout
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
"${TARLZ}" --un -q -rf aout.tar.lz foo bar baz		# contradictory ext
[ $? = 1 ] || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
cp "${eoa_lz}" aout.tar.lz || framework_failure  # append to empty archive
"${TARLZ}" -0 -rf aout.tar.lz foo bar baz || test_failed $LINENO
cmp out.tar.lz aout.tar.lz || test_failed $LINENO
rm -f out.tar.lz aout.tar.lz || framework_failure

# test --append --uncompressed
"${TARLZ}" -cf out.tar foo bar baz || test_failed $LINENO
"${TARLZ}" -cf aout.tar foo || test_failed $LINENO
"${TARLZ}" -rf aout.tar foo bar baz --exclude foo || test_failed $LINENO
is_uncompressed out.tar || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
rm -f aout.tar || framework_failure
touch aout.tar empty || framework_failure	# contradictory ext empty file
"${TARLZ}" -0 -q -rf aout.tar foo bar baz
[ $? = 1 ] || test_failed $LINENO
cmp aout.tar empty || test_failed $LINENO
rm -f aout.tar empty || framework_failure
touch aout.tar || framework_failure		# append to empty file
"${TARLZ}" -rf aout.tar foo bar baz || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -rf aout.tar || test_failed $LINENO		# append nothing
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -rf aout.tar -C nx_dir || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -q -rf aout.tar nx_file
[ $? = 1 ] || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -q -rf aout.tar aout.tar || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" --un -r foo bar baz > aout.tar || test_failed $LINENO  # to stdout
cmp out.tar aout.tar || test_failed $LINENO
"${TARLZ}" -0 -q -rf aout.tar foo bar baz		# contradictory ext
[ $? = 1 ] || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
cp "${eoa}" aout.tar || framework_failure	# append to empty archive
"${TARLZ}" -rf aout.tar foo bar baz || test_failed $LINENO
cmp out.tar aout.tar || test_failed $LINENO
rm -f out.tar aout.tar || framework_failure

# test --append to solid archive
"${TARLZ}" --solid -q -0 -cf out.tar.lz "${in}" foo bar || test_failed $LINENO
"${TARLZ}" -q -tf out.tar.lz || test_failed $LINENO	# compressed seekable
cp out.tar.lz aout.tar.lz || framework_failure
for i in --asolid --bsolid --dsolid --solid -0 ; do
	"${TARLZ}" $i -q -rf out.tar.lz baz
	[ $? = 2 ] || test_failed $LINENO $i
	cmp out.tar.lz aout.tar.lz || test_failed $LINENO $i
done
rm -f out.tar.lz aout.tar.lz || framework_failure
for i in --asolid --bsolid --dsolid -0 ; do
  for j in --asolid --bsolid --dsolid --solid -0 ; do
    "${TARLZ}" $i -0 -cf out.tar.lz foo || test_failed $LINENO "$i $j"
    "${TARLZ}" $j -0 -rf out.tar.lz bar baz || test_failed $LINENO "$i $j"
    rm -f foo bar baz || framework_failure
    "${TARLZ}" -xf out.tar.lz || test_failed $LINENO "$i $j"
    cmp cfoo foo || test_failed $LINENO "$i $j"
    cmp cbar bar || test_failed $LINENO "$i $j"
    cmp cbaz baz || test_failed $LINENO "$i $j"
    rm -f out.tar.lz || framework_failure
  done
done
rm -f foo bar baz || framework_failure

printf "\ntesting dirs and links..."

# test -c -d -x on directories and links
mkdir dir1 || framework_failure
"${TARLZ}" -0 -cf out.tar.lz dir1 || test_failed $LINENO
rmdir dir1 || framework_failure
"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
[ -d dir1 ] || test_failed $LINENO
rmdir dir1
rm -f out.tar.lz || framework_failure
mkdir dir1 || framework_failure
"${TARLZ}" -cf out.tar dir1 || test_failed $LINENO
is_uncompressed out.tar || test_failed $LINENO
rmdir dir1 || framework_failure
"${TARLZ}" -xf out.tar || test_failed $LINENO
[ -d dir1 ] || test_failed $LINENO
rmdir dir1
rm -f out.tar || framework_failure

if [ "${ln_works}" = yes ] ; then
	name_100=name_100_bytes_long_nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
	path_100=dir1/dir2/dir3/path_100_bytes_long_nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
	path_106=dir1/dir2/dir3/path_longer_than_100_bytes_nnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnnn
	mkdir dir1 || framework_failure
	mkdir dir1/dir2 || framework_failure
	mkdir dir1/dir2/dir3 || framework_failure
	cp "${in}" dir1/dir2/dir3/in || framework_failure
	ln dir1/dir2/dir3/in dir1/dir2/dir3/"${name_100}" || framework_failure
	ln dir1/dir2/dir3/in "${path_100}" || framework_failure
	ln dir1/dir2/dir3/in "${path_106}" || framework_failure
	ln -s dir2/ dir1/dir2_link || framework_failure
	ln -s in dir1/dir2/dir3/link || framework_failure
	ln -s "${name_100}" dir1/dir2/dir3/link_100 || framework_failure
	"${TARLZ}" -0 -cf out.tar.lz dir1 || test_failed $LINENO
	"${TARLZ}" -df out.tar.lz || test_failed $LINENO
	rm -rf dir1 || framework_failure
	"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
	"${TARLZ}" -df out.tar.lz || test_failed $LINENO
	cmp "${in}" dir1/dir2/dir3/in || test_failed $LINENO
	cmp "${in}" dir1/dir2_link/dir3/in || test_failed $LINENO
	cmp "${in}" dir1/dir2/dir3/"${name_100}" || test_failed $LINENO
	cmp "${in}" "${path_100}" || test_failed $LINENO
	cmp "${in}" "${path_106}" || test_failed $LINENO
	cmp "${in}" dir1/dir2/dir3/link || test_failed $LINENO
	cmp "${in}" dir1/dir2/dir3/link_100 || test_failed $LINENO
	rm -f dir1/dir2/dir3/in || framework_failure
	cmp "${in}" dir1/dir2/dir3/link 2> /dev/null && test_failed $LINENO
	cmp "${in}" dir1/dir2/dir3/link_100 || test_failed $LINENO
	"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
	rm -f out.tar.lz || framework_failure
	cmp "${in}" dir1/dir2/dir3/in || test_failed $LINENO
	cmp "${in}" dir1/dir2/dir3/link || test_failed $LINENO
	"${TARLZ}" -0 -q -c ../tmp/dir1 | "${TARLZ}" -x || test_failed $LINENO
	diff -ru tmp/dir1 dir1 || test_failed $LINENO
	rm -rf tmp dir1 || framework_failure
	# test -c -d -x on dangling (broken) symlinks with trailing slashes
	"${TARLZ}" -xf "${testdir}"/ts_in_link.tar.lz || test_failed $LINENO
	"${TARLZ}" -df "${testdir}"/ts_in_link.tar.lz --ignore-ids ||
		test_failed $LINENO
	"${TARLZ}" -0 -cf out.tar.lz link1 link2 link3 link4 || test_failed $LINENO
	"${TARLZ}" -df out.tar.lz || test_failed $LINENO
	rm -f out.tar.lz link1 link2 link3 link4 || framework_failure
fi

printf "\ntesting long names..."

"${TARLZ}" -q -tf "${t155}" || test_failed $LINENO
"${TARLZ}" -q -tf "${t155_lz}" || test_failed $LINENO
if [ "${ln_works}" = yes ] ; then
	mkdir dir1 || framework_failure
	"${TARLZ}" -C dir1 -xf "${t155}" || test_failed $LINENO
	mkdir dir2 || framework_failure
	"${TARLZ}" -C dir2 -xf "${t155_lz}" || test_failed $LINENO
	diff -ru dir1 dir2 || test_failed $LINENO
	"${TARLZ}" -cf out.tar.lz dir2 || test_failed $LINENO
	rm -rf dir2 || framework_failure
	"${TARLZ}" -xf out.tar.lz || test_failed $LINENO
	diff -ru dir1 dir2 || test_failed $LINENO
	rmdir dir2 2> /dev/null && test_failed $LINENO
	rmdir dir1 2> /dev/null && test_failed $LINENO
	rm -rf out.tar.lz dir2 dir1 || framework_failure
fi

"${TARLZ}" -tvf "${testdir}"/ug32chars.tar.lz | grep -q \
	-e very_long_owner_name_of_32_chars/very_long_group_name_of_32_chars ||
	test_failed $LINENO
"${TARLZ}" -tvf "${testdir}"/ug32chars.tar.lz | grep -q \
	-e very_long_owner_name_of_32_charsvery_long_group_name_of_32_chars &&
	test_failed $LINENO
"${TARLZ}" -tvf "${testdir}"/ug32chars.tar.lz | grep -q \
	-e very_long_group_name_of_32_chars/very_long_group_name_of_32_chars &&
	test_failed $LINENO
"${TARLZ}" -xf "${testdir}"/ug32chars.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
rm -f foo || framework_failure
"${TARLZ}" -tvf "${testdir}"/ug32767.tar.lz | grep -q -e 32767/32767 ||
	test_failed $LINENO
"${TARLZ}" -xf "${testdir}"/ug32767.tar.lz || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
rm -f foo || framework_failure

printf "\ntesting --compress..."

cp cfoo foo || framework_failure
cp cbar bar || framework_failure
cp cbaz baz || framework_failure
cp "${in}" test.txt || framework_failure
"${TARLZ}" -cf out.tar test.txt foo bar baz test.txt || test_failed $LINENO
"${TARLZ}" -cf out3.tar foo bar baz || test_failed $LINENO
cp out.tar outz.tar || framework_failure
cp out3.tar out3z.tar || framework_failure
#
"${TARLZ}" -0 -z outz.tar out3z.tar || test_failed $LINENO
"${TARLZ}" -q -tf outz.tar.lz || test_failed $LINENO
"${TARLZ}" -q -tf out3z.tar.lz || test_failed $LINENO
cp outz.tar.lz out || test_failed $LINENO
cp out3z.tar.lz out3 || test_failed $LINENO
rm -f out3z.tar.lz || framework_failure
"${TARLZ}" -q -0 -z outz.tar out3z.tar		# outz.tar.lz exists
[ $? = 1 ] || test_failed $LINENO
cmp out outz.tar.lz || test_failed $LINENO
cmp out3 out3z.tar.lz || test_failed $LINENO
if [ "${ln_works}" = yes ] ; then
	ln -s outz.tar loutz.tar || framework_failure
	"${TARLZ}" -0 -z loutz.tar || test_failed $LINENO
	cmp loutz.tar.lz outz.tar.lz || test_failed $LINENO
	rm -f loutz.tar.lz loutz.tar || framework_failure
fi
rm -f out out3 outz.tar.lz out3z.tar.lz || framework_failure
#
for i in --solid --no-solid ; do
  "${TARLZ}" -0 -n0 $i -cf out.tar.lz test.txt foo bar baz test.txt || test_failed $LINENO $i
  "${TARLZ}" -0 -z -o - $i out.tar | cmp out.tar.lz - || test_failed $LINENO $i
  "${TARLZ}" -0 -n0 $i -cf out3.tar.lz foo bar baz || test_failed $LINENO $i
  "${TARLZ}" -0 -z -o - $i out3.tar | cmp out3.tar.lz - || test_failed $LINENO $i
  "${TARLZ}" -0 -z $i outz.tar out3z.tar || test_failed $LINENO $i
  cmp out.tar.lz outz.tar.lz || test_failed $LINENO $i
  cmp out3.tar.lz out3z.tar.lz || test_failed $LINENO $i
  rm -f outz.tar.lz out3z.tar.lz || framework_failure
done
#
"${TARLZ}" -0 -B8KiB -n0 --bsolid -cf out.tar.lz test.txt foo bar baz test.txt || test_failed $LINENO
"${TARLZ}" -0 -B8KiB -z -o - --bsolid out.tar | cmp out.tar.lz - || test_failed $LINENO
"${TARLZ}" -0 -B8KiB -z -o out --bsolid out.tar || test_failed $LINENO
cmp out.tar.lz out || test_failed $LINENO
"${TARLZ}" -0 -B8KiB -z --bsolid outz.tar || test_failed $LINENO
cmp out.tar.lz outz.tar.lz || test_failed $LINENO
rm -f out outz.tar.lz || framework_failure
"${TARLZ}" -0 -B8KiB -z -o a/b/c/out --bsolid out.tar || test_failed $LINENO
cmp out.tar.lz a/b/c/out || test_failed $LINENO
rm -rf a || framework_failure
#
"${TARLZ}" -0 -n0 --asolid -cf out.tar.lz test.txt foo bar baz test.txt || test_failed $LINENO
"${TARLZ}" -0 -n0 --asolid -cf out3.tar.lz foo bar baz || test_failed $LINENO
for i in --asolid --bsolid --dsolid ; do
  cat out.tar | "${TARLZ}" -0 -z $i | cmp out.tar.lz - || test_failed $LINENO $i
  "${TARLZ}" -0 -z -o out $i out.tar || test_failed $LINENO $i
  cmp out.tar.lz out || test_failed $LINENO $i
  "${TARLZ}" -0 -z $i outz.tar out3z.tar || test_failed $LINENO $i
  cmp out.tar.lz outz.tar.lz || test_failed $LINENO $i
  cmp out3.tar.lz out3z.tar.lz || test_failed $LINENO $i
  rm -f out outz.tar.lz out3z.tar.lz || framework_failure
done
# concatenate and compress
"${TARLZ}" -cf foo.tar foo || test_failed $LINENO
"${TARLZ}" -cf bar.tar bar || test_failed $LINENO
"${TARLZ}" -cf baz.tar baz || test_failed $LINENO
"${TARLZ}" -A foo.tar bar.tar baz.tar | "${TARLZ}" -0 -z -o foobarbaz.tar.lz ||
  test_failed $LINENO
cmp out3.tar.lz foobarbaz.tar.lz || test_failed $LINENO
"${TARLZ}" -A foo.tar bar.tar baz.tar | "${TARLZ}" -0 -z > foobarbaz.tar.lz ||
  test_failed $LINENO
cmp out3.tar.lz foobarbaz.tar.lz || test_failed $LINENO
# compress and concatenate
"${TARLZ}" -0 -z foo.tar bar.tar baz.tar || test_failed $LINENO
"${TARLZ}" -A foo.tar.lz bar.tar.lz baz.tar.lz > foobarbaz.tar.lz ||
  test_failed $LINENO
"${TARLZ}" -0 -n0 --no-solid -c foo bar baz | cmp foobarbaz.tar.lz - ||
  test_failed $LINENO
rm -f foo bar baz test.txt out.tar.lz out.tar outz.tar foobarbaz.tar.lz \
      out3.tar out3.tar.lz out3z.tar foo.tar bar.tar baz.tar \
      foo.tar.lz bar.tar.lz baz.tar.lz || framework_failure

printf "\ntesting bad input..."

# test --extract ".."
mkdir dir1 || framework_failure
cd dir1 || framework_failure
for i in 0 2 ; do		# try serial and parallel decoders
  "${TARLZ}" -q -n$i -xf "${testdir}"/dotdot1.tar.lz || test_failed $LINENO $i
  [ ! -e ../dir ] || test_failed $LINENO $i
  "${TARLZ}" -q -n$i -xf "${testdir}"/dotdot2.tar.lz || test_failed $LINENO $i
  [ ! -e ../dir ] || test_failed $LINENO $i
  "${TARLZ}" -q -n$i -xf "${testdir}"/dotdot3.tar.lz || test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
  "${TARLZ}" -q -n$i -xf "${testdir}"/dotdot4.tar.lz || test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
  "${TARLZ}" -q -n$i -xf "${testdir}"/dotdot5.tar.lz || test_failed $LINENO $i
  [ ! -e dir ] || test_failed $LINENO $i
done
cd .. || framework_failure
rm -rf dir1 || framework_failure

# test --list and --extract truncated tar
dd if="${in_tar}" of=truncated.tar bs=1000 count=1 2> /dev/null
"${TARLZ}" -q -tf truncated.tar > /dev/null
[ $? = 2 ] || test_failed $LINENO
"${TARLZ}" -q -xf truncated.tar
[ $? = 2 ] || test_failed $LINENO
[ ! -e test.txt ] || test_failed $LINENO
rm -f truncated.tar || framework_failure

# test --delete with split 'bar' tar member
for i in 1 2 3 4 ; do
  cp "${testdir}"/test3_sm${i}.tar.lz out.tar.lz || framework_failure
  for j in bar baz ; do
    "${TARLZ}" -q -f out.tar.lz --delete $j
    [ $? = 2 ] || test_failed $LINENO "$i $j"
  done
  cmp "${testdir}"/test3_sm${i}.tar.lz out.tar.lz || test_failed $LINENO $i
  "${TARLZ}" -q -f out.tar.lz --delete foo
  [ $? = 2 ] || test_failed $LINENO $i
  "${TARLZ}" -xf out.tar.lz || test_failed $LINENO $i
  [ ! -e foo ] || test_failed $LINENO $i
  cmp cbar bar || test_failed $LINENO $i
  cmp cbaz baz || test_failed $LINENO $i
  rm -f out.tar.lz foo bar baz || framework_failure
done

# test --list and --extract format violations
if [ "${ln_works}" = yes ] ; then
	mkdir dir1 || framework_failure
	"${TARLZ}" -C dir1 -xf "${t155}" || test_failed $LINENO
fi
for i in 1 2 3 ; do
  "${TARLZ}" -q -tf "${testdir}"/t155_fv${i}.tar
  [ $? = 2 ] || test_failed $LINENO $i
  "${TARLZ}" -q -tf "${testdir}"/t155_fv${i}.tar --permissive ||
    test_failed $LINENO $i
  if [ "${ln_works}" = yes ] ; then
    mkdir dir2 || framework_failure
    "${TARLZ}" -C dir2 -xf "${testdir}"/t155_fv${i}.tar --permissive ||
      test_failed $LINENO $i
    diff -ru dir1 dir2 || test_failed $LINENO $i
    rm -rf dir2 || framework_failure
  fi
done
for i in 1 2 3 4 5 6 ; do
  "${TARLZ}" -q -tf "${testdir}"/t155_fv${i}.tar.lz
  [ $? = 2 ] || test_failed $LINENO $i
  "${TARLZ}" -q -tf "${testdir}"/t155_fv${i}.tar.lz --permissive ||
    test_failed $LINENO $i
  if [ "${ln_works}" = yes ] ; then
    mkdir dir2 || framework_failure
    "${TARLZ}" -n4 -C dir2 -xf "${testdir}"/t155_fv${i}.tar.lz --permissive ||
      test_failed $LINENO $i
    diff -ru dir1 dir2 || test_failed $LINENO $i
    rm -rf dir2 || framework_failure
  fi
done
if [ "${ln_works}" = yes ] ; then rm -rf dir1 || framework_failure ; fi

for i in "${testdir}"/test3_nn.tar "${testdir}"/test3_nn.tar.lz ; do
  "${TARLZ}" -q -n0 -tf "$i" || test_failed $LINENO "$i"
  "${TARLZ}" -q -n4 -tf "$i" || test_failed $LINENO "$i"
  "${TARLZ}" -q -n0 -xf "$i" || test_failed $LINENO "$i"
  if [ "${d_works}" = yes ] ; then
    "${TARLZ}" -n0 -df "$i" --ignore-ids || test_failed $LINENO "$i"
  fi
  cmp cfoo foo || test_failed $LINENO "$i"
  [ ! -e bar ] || test_failed $LINENO "$i"
  cmp cbaz baz || test_failed $LINENO "$i"
  rm -f foo bar baz || framework_failure
  "${TARLZ}" -q -n4 -xf "$i" || test_failed $LINENO "$i"
  if [ "${d_works}" = yes ] ; then
    "${TARLZ}" -n4 -df "$i" --ignore-ids || test_failed $LINENO "$i"
  fi
  cmp cfoo foo || test_failed $LINENO "$i"
  [ ! -e bar ] || test_failed $LINENO "$i"
  cmp cbaz baz || test_failed $LINENO "$i"
  rm -f foo bar baz || framework_failure
done

printf "\ntesting --keep-damaged..."

# test --extract and --keep-damaged compressed
rm -f test.txt || framework_failure
for i in "${inbad1}" "${inbad2}" ; do
	"${TARLZ}" -q -xf "${i}.tar.lz"
	[ $? = 2 ] || test_failed $LINENO "$i"
	[ ! -e test.txt ] || test_failed $LINENO "$i"
	rm -f test.txt || framework_failure
	"${TARLZ}" -q -n0 -xf "${i}.tar.lz" --keep-damaged
	[ $? = 2 ] || test_failed $LINENO "$i"
	[ -e test.txt ] || test_failed $LINENO "$i"
	cmp "$i" test.txt 2> /dev/null || test_failed $LINENO "$i"
	rm -f test.txt || framework_failure
done
#
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad1_lz}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad2_lz}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad3_lz}"
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad3_lz}" --keep-damaged
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar 2> /dev/null || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad4_lz}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad4_lz}" --keep-damaged
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
cmp cbar bar 2> /dev/null || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad5_lz}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad5_lz}" --keep-damaged
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo 2> /dev/null || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -n0 -xf "${bad6_lz}"
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO

# test --extract and --keep-damaged uncompressed
rm -f test.txt || framework_failure
"${TARLZ}" -q -xf "${inbad1}.tar"
[ $? = 2 ] || test_failed $LINENO
[ ! -e test.txt ] || test_failed $LINENO
rm -f test.txt || framework_failure
"${TARLZ}" -q -xf "${inbad1}.tar" --keep-damaged
[ $? = 2 ] || test_failed $LINENO
[ -e test.txt ] || test_failed $LINENO
cmp "${inbad1}" test.txt 2> /dev/null || test_failed $LINENO
rm -f test.txt || framework_failure
#
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf "${bad1}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf "${bad2}"
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf "${bad3}"
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf "${bad4}"
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
rm -f foo bar baz || framework_failure
"${TARLZ}" -q -xf "${bad5}"
[ $? = 2 ] || test_failed $LINENO
cmp cfoo foo || test_failed $LINENO
cmp cbar bar || test_failed $LINENO
cmp cbaz baz || test_failed $LINENO
rm -f cfoo cbar cbaz foo bar baz || framework_failure
#
rm -f test3.tar.lz || framework_failure
"${TARLZ}" -q -xf "${tlzit1}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
[ ! -e test3.tar.lz ] || test_failed $LINENO
rm -f foo bar baz test3.tar.lz || framework_failure
"${TARLZ}" -q -xf "${tlzit2}"
[ $? = 2 ] || test_failed $LINENO
[ ! -e foo ] || test_failed $LINENO
[ ! -e bar ] || test_failed $LINENO
[ ! -e baz ] || test_failed $LINENO
cmp "${test3_lz}" test3.tar.lz || test_failed $LINENO
rm -f foo bar baz test3.tar.lz || framework_failure

echo
if [ ${fail} = 0 ] ; then
	echo "tests completed successfully."
	cd "${objdir}" && rm -r tmp
else
	echo "tests failed."
fi
exit ${fail}
