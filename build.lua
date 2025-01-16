-- This file is part of lzip-builder.
--
-- lzip-builder is free software: you can redistribute it and/or modify
-- it under the terms of the GNU General Public License as published by
-- the Free Software Foundation, either version 3 of the License, or
-- (at your option) any later version.
--
-- lzip-builder is distributed in the hope that it will be useful,
-- but WITHOUT ANY WARRANTY; without even the implied warranty of
-- MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
-- GNU General Public License for more details.
--
-- You should have received a copy of the GNU General Public License
-- along with lzip-builder.  If not, see <https://www.gnu.org/licenses/>.
--
-- For further information about lzip-builder you can visit
-- https://github.com/CDSoft/lzip-builder

var "release" "2025-01-15"

var "lzlib_version"         "1.15"
var "lzip_version"          "1.24.1"
var "plzip_version"         "1.12"
var "tarlz_version"         "0.26"
var "lziprecover_version"   "1.25"

help.name "lzip-builder"
help.description [[
Distribution of lzip binaries for Linux, MacOS and Windows.

This Ninja build file will compile and install:

- lzip          $lzip_version
- plzip         $plzip_version
- tarlz         $tarlz_version
- lziprecover   $lziprecover_version

Note that only lzip is supported on Windows.
]]

local F = require "F"
local targets = require "targets"

var "builddir" ".build"
clean "$builddir"

var "tmp" "$builddir/tmp"
var "bin" "$builddir/bin"
var "all" "$builddir/all"

rule "extract" {
    description = "extract $url",
    command = "curl -fsSL $url | PATH=$bin:$$PATH tar x --$fmt $opt",
}

local cflags = {
    "-O3",
    "-s",
    "-Ilzlib-$lzlib_version",
    [[-DPROGVERSION='"$progversion"']],
}

local ldflags = {
    "-s",
}

build.cpp : add "cflags" { cflags } : add "ldflags" { ldflags }
targets : foreach(function(target)
    build.zigcpp[target.name] : add "cflags" { cflags } : add "ldflags" { ldflags }
end)

local host = {}         -- binaries for the current host compiled with cc/c++
local cross = targets   -- binaries for all supported platforms compiled with zig
    : map(function(target) return {target.name, {}} end)
    : from_list()

--------------------------------------------------------------------
section "lzlib"
--------------------------------------------------------------------

var "lzlib_url" "http://download.savannah.gnu.org/releases/lzip/lzlib/lzlib-$lzlib_version.tar.gz"

local lzlib_implicit_sources = F.map(F.prefix("lzlib-$lzlib_version/"), {
    "cbuffer.c",
    "decoder.c",
    "encoder.c",
    "encoder_base.c",
    "fast_encoder.c",
})

local lzlib_sources = F.map(F.prefix("lzlib-$lzlib_version/"), {
    "lzlib.c",
})

build{lzlib_sources, lzlib_implicit_sources} { "extract",
    url = "$lzlib_url",
    fmt = "gzip",
    opt = {
        '--exclude="doc/*"',
        '--exclude="testsuite/*"',
    },
}

--------------------------------------------------------------------
section "lzip"
--------------------------------------------------------------------

var "lzip_url" "http://download.savannah.gnu.org/releases/lzip/lzip-$lzip_version.tar.gz"

local lzip_sources = F.map(F.prefix("lzip-$lzip_version/"), {
    "arg_parser.cc",
    "decoder.cc",
    "encoder.cc",
    "encoder_base.cc",
    "fast_encoder.cc",
    "list.cc",
    "lzip_index.cc",
    "main.cc",
})

build(lzip_sources) { "extract",
    url = "$lzip_url",
    fmt = "gzip",
    opt = {
        '--exclude="doc/*"',
        '--exclude="testsuite/*"',
    },
}

local lzip = build.cpp("$bin/lzip") {
    progversion = "$lzip_version",
    lzip_sources,
}
acc(host) { lzip }

targets : foreach(function(target)
    acc(cross[target.name]) {
        build.zigcpp[target.name]("$all"/target.name/"lzip") {
            progversion = "$lzip_version",
            lzip_sources,
        }
    }
end)

--------------------------------------------------------------------
section "plzip"
--------------------------------------------------------------------

var "plzip_url" "http://download.savannah.gnu.org/releases/lzip/plzip/plzip-$plzip_version.tar.lz"

local plzip_sources = F.map(F.prefix("plzip-$plzip_version/"), {
    "arg_parser.cc",
    "compress.cc",
    "dec_stdout.cc",
    "dec_stream.cc",
    "decompress.cc",
    "list.cc",
    "lzip_index.cc",
    "main.cc",
})

build(plzip_sources) { "extract",
    url = "$plzip_url",
    fmt = "lzip",
    order_only_deps = lzip,
    opt = {
        '--exclude="doc/*"',
        '--exclude="testsuite/*"',
    },
}

acc(host) {
    build.cpp("$bin/plzip") {
        progversion = "$plzip_version",
        plzip_sources,
        lzlib_sources,
        implicit_in = lzlib_implicit_sources,
    }
}

targets : foreach(function(target)
    acc(cross[target.name]) {
        target.os == "windows" and {}
        or build.zigcpp[target.name]("$all"/target.name/"plzip"..target.exe) {
            progversion = "$plzip_version",
            plzip_sources,
            lzlib_sources,
            implicit_in = lzlib_implicit_sources,
        }
    }
end)

--------------------------------------------------------------------
section "tarlz"
--------------------------------------------------------------------

var "tarlz_url" "http://download.savannah.gnu.org/releases/lzip/tarlz/tarlz-$tarlz_version.tar.lz"

local tarlz_sources = F.map(F.prefix("tarlz-$tarlz_version/"), {
    "archive_reader.cc",
    "arg_parser.cc",
    "common.cc",
    "common_decode.cc",
    "common_mutex.cc",
    "compress.cc",
    "create.cc",
    "create_lz.cc",
    "decode.cc",
    "decode_lz.cc",
    "delete.cc",
    "delete_lz.cc",
    "exclude.cc",
    "extended.cc",
    "lzip_index.cc",
    "main.cc",
})

build(tarlz_sources) { "extract",
    url = "$tarlz_url",
    fmt = "lzip",
    order_only_deps = lzip,
    opt = {
        '--exclude="doc/*"',
        '--exclude="testsuite/*"',
    },
}

acc(host) {
    build.cpp("$bin/tarlz") {
        progversion = "$tarlz_version",
        tarlz_sources,
        lzlib_sources,
        implicit_in = lzlib_implicit_sources,
    }
}

targets : foreach(function(target)
    acc(cross[target.name]) {
        target.os == "windows" and {}
        or build.zigcpp[target.name]("$all"/target.name/"tarlz"..target.exe) {
            progversion = "$tarlz_version",
            tarlz_sources,
            lzlib_sources,
            implicit_in = lzlib_implicit_sources,
        }
    }
end)

--------------------------------------------------------------------
section "lziprecover"
--------------------------------------------------------------------

var "lziprecover_url" "http://download.savannah.gnu.org/releases/lzip/lziprecover/lziprecover-$lziprecover_version.tar.lz"

local lziprecover_implicit_sources = F.map(F.prefix("lziprecover-$lziprecover_version/"), {
    "main_common.cc",
})
local lziprecover_sources = F.map(F.prefix("lziprecover-$lziprecover_version/"), {
    "alone_to_lz.cc",
    "arg_parser.cc",
    "byte_repair.cc",
    "decoder.cc",
    "dump_remove.cc",
    "fec_create.cc",
    "fec_repair.cc",
    "gf8.cc",
    "gf16.cc",
    "list.cc",
    "lunzcrash.cc",
    "lzip_index.cc",
    "main.cc",
    "md5.cc",
    "merge.cc",
    "mtester.cc",
    "nrep_stats.cc",
    "range_dec.cc",
    "recursive.cc",
    "reproduce.cc",
    "split.cc",
})

build{lziprecover_sources, lziprecover_implicit_sources} { "extract",
    url = "$lziprecover_url",
    fmt = "lzip",
    order_only_deps = lzip,
    opt = {
        '--exclude="doc/*"',
        '--exclude="testsuite/*"',
    },
}

acc(host) {
    build.cpp("$bin/lziprecover") {
        progversion = "$lziprecover_version",
        lziprecover_sources,
        lzlib_sources,
        implicit_in = {
            lziprecover_implicit_sources,
            lzlib_implicit_sources,
        },
    }
}

targets : foreach(function(target)
    acc(cross[target.name]) {
        target.os == "windows" and {}
        or build.zigcpp[target.name]("$all"/target.name/"lziprecover") {
            progversion = "$lziprecover_version",
            lziprecover_sources,
            lzlib_sources,
            implicit_in = lzlib_implicit_sources,
        }
    }
end)

--------------------------------------------------------------------
section "Host binaries"
--------------------------------------------------------------------

phony "compile" { host }
help "compile" "Build Lzip binaries for the host only"

default "compile"

install "bin" { host }

--------------------------------------------------------------------
section "Archives"
--------------------------------------------------------------------

rule "tar" {
    description = "tar $out",
    command = "tar -caf $out $in --transform='s,$prefix/,,'",
}

local archives = targets : map(function(target)
    return build("$builddir/lzip-build-${release}-"..target.name..".tar.gz") { "tar",
        cross[target.name],
        prefix = "$all"/target.name,
    }
end)

local release_note = build "$builddir/README.md" {
    description = "$out",
    command = {
        "(",
        'echo "# Lzip ${release}";',
        'echo "";',
        'echo "| Program | Version | Documentation |";',
        'echo "| ------- | ------- | ------------- |";',
        'echo "| lzip | [$lzip_version]($lzip_url) | <https://www.nongnu.org/lzip/> |";',
        'echo "| plzip | [$plzip_version]($plzip_url) | <https://www.nongnu.org/lzip/plzip.html> |";',
        'echo "| tarlz | [$tarlz_version]($tarlz_url) | <https://www.nongnu.org/lzip/tarlz.html> |";',
        'echo "| lziprecover | [$lziprecover_version]($lziprecover_url) | <https://www.nongnu.org/lzip/lziprecover.html> |";',
        ") > $out",
    },
}

phony "all" { archives, release_note }
help "all" "Build Lzip archives for Linux, MacOS and Windows"
