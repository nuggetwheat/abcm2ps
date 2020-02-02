# Chords

## Overview

This is an extension of abcm2ps that can generate chord listing output.  To
generate this output, you supply the `--aux-output <mode>` option, where
`<mode>` can be one of:
- *chords*  Generates a `chords.html` file containing condensed song chord
  listings for ryhthm players
- *irealpro*  Generates an `irealpro.html` file that can be imported into the
  [iReal Pro app](https://irealpro.com/) to provide backing tracks
- *complexity*  Generates a `comlexity.csv` file that can be imported into
  Excel or Sheets with one row per song and columns containing song complexity
  signals

## Input

The input to abcm2ps with chord listing extensions can be a file that represents
the concatenation of many songs.  When concatenating songs to be used as input,
make sure there is a newline (`\n`) character between each song.  Some abc files
don't contain a trailing newline so if you do a simple concatenation with the
`cat` command, it will cause the subdsequent song to be ignored.  This is
because the reference number field `X:` of the subsequent song wont end up at
the beginning of a line.  To get around this, use a concatenation script such as
the following `concat.abc.sh`:

```
#!/bin/bash

PATTERN=

if [ $# -eq  1 ]; then
    PATTERN="$1"
fi

for f in $PATTERN ; do
    cat $f
    echo
done
```

The following is an example command line illustrating how it can be used:

```
concat_abc.sh "/Users/doug/personal/music/abc/*/*.abc" > ~/all.abc
```

### Mode: chords

This output mode will generate a `chords.html` file containing a condensed chord
listing for each of the input songs.  It only works for tunes that include chord
symbols.  The following is an example command that will generate a chord listing
file from a concatenation of abc songs in the file `~/bluegrass_jam.abc`:

```
./abcm2ps -aux-output chords -aux-flag title "Bluegrass Jam" ~/bluegrass_jam.abc
```

**NOTE:** The reference number field `X:` is only used to signal the beginning
of a new song.  The reference number itself is not used.  In other words, the
reference number fields in a concatenated file of abc tunes can all be the same
(e.g. they can all be `X:1`).

### Mode: irealpro

This output mode will generate an `irealpro.html` file that contains backing
tracks for all of the input songs.  To avoid song title collisions in iReal Pro,
you can modify the song titles with a unique prefix by supplying the
`-aux-flag song-title-prefix <prefix>` option.  The following is an example
command line for this output mode:

```
./abcm2ps -aux-output irealpro \
	  -aux-flag song-title-prefix "[bgjam]" \
	  -aux-flag title "Bluegrass Jam" ~/bluegrass_jam.abc
```

To import the `irealpro.html` file into iReal Pro, double-click on it in the Mac
Finder App to import it into iReal Pro installed on a mac.  To import it into
iReal Pro installed on your iPhone or Android device, mail the file to yourself
as an attachment and then open the attachment on your phone.

### Mode: complexity

This output mode will generate a `complexity.csv` file containing one line per
song of comma-separated fields that include complexity signals.  The fields
include:
- Key
- Time Signature
- Notes per Beat
- Average Long Interval
- Song Title

The following is an example command line for this output mode:

```
./abcm2ps -aux-output complexity ~/songs.abc
```

