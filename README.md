delta is a library (and an associated utility) which allow
us to perform binary diff and patching in an efficient way. It
is similar in function to xdelta and vcdiff (RFC 3284), but more
(space-)efficient.

	-rw-r--r--  1 agc  users  10107 Apr 14 10:41 1
	-rw-r--r--  1 agc  users    206 Apr 19 16:50 1.bsd
	-rw-r--r--  1 agc  users    109 Apr 27 22:11 1.diff
	-rw-r--r--  1 agc  users    222 Apr 19 16:48 1.xd
	-rw-r--r--  1 agc  users    158 Apr 19 17:48 1.xd3
	-rw-r--r--  1 agc  users  10113 Apr 14 10:41 2
	-rw-r--r--  1 agc  users  10113 Apr 27 22:11 2.new

The delta(1) utility takes 3 arguments - oldfile, newfile and
patchfile - and creates the patchfile from the binary difference of
the old and new files. There's an optional -d or -p to diff and
to patch, respectively. The default is to patch.

	% delta -d 2 1 2.diff
	% delta -p 2 1.new 2.diff
	% diff 1 1.new
	%

The patchfile uses bzip2, and is more efficient than both xdelta (v1
and v3) by Josh MacDonald, and bsdiff from Colin Percival, dated 2005. 
It also uses zigzag encoding for the numbers stored internally, in
order to be space-efficient.

I also tried using xz/lzma compression, but the results were not good
- around 160 bytes for a delta file where bzip2 ended up with 111
bytes.
