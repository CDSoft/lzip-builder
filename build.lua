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

var "release" "lzip-build-r1"

var "lzlib_version"         "1.14"
var "lzip_version"          "1.24.1"
var "plzip_version"         "1.11"
var "tarlz_version"         "0.26"
var "lziprecover_version"   "1.24"

help.name "lzip-builder"
help.description [[
Distribution of lzip binaries for Linux, MacOS and Windows.

This Ninja build file will compile and install:

- lzip          $lzip_version
- plzip         $plzip_version
- tarlz         $tarlz_version
- lziprecovery  $lziprecover_version

Note that only lzip is supported on Windows.
]]

local F = require "F"
local targets = require "targets"

var "builddir" ".build"
clean "$builddir"

rule "extract" {
    description = "extract $url",
    command = "curl -fsSL $url | PATH=$builddir:$$PATH tar x --$fmt",
}

local cflags = {
    "-O3",
    "-s",
    "-Ilzlib-$lzlib_version",
    [[-DPROGVERSION='"$progversion"']],
}

build.cpp : add "cflags" { cflags }
targets : foreach(function(target)
    build.zigcpp[target.name] : add "cflags" { cflags }
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

build{lzlib_sources, lzlib_implicit_sources} { "extract", url="$lzlib_url", fmt="gzip" }

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

build(lzip_sources) { "extract", url="$lzip_url", fmt="gzip" }

local lzip = build.cpp("$builddir/lzip") {
    progversion = "$lzip_version",
    lzip_sources,
}
acc(host) { lzip }

targets : foreach(function(target)
    acc(cross[target.name]) {
        build.zigcpp[target.name]("$builddir"/target.name/"lzip") {
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

build(plzip_sources) { "extract", url="$plzip_url", fmt="lzip", order_only_deps=lzip }

acc(host) {
    build.cpp("$builddir/plzip") {
        progversion = "$plzip_version",
        plzip_sources,
        lzlib_sources,
        implicit_in = lzlib_implicit_sources,
    }
}

targets : foreach(function(target)
    acc(cross[target.name]) {
        target.os == "windows" and {}
        or build.zigcpp[target.name]("$builddir"/target.name/"plzip"..target.exe) {
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

build(tarlz_sources) { "extract", url="$tarlz_url", fmt="lzip", order_only_deps=lzip }

acc(host) {
    build.cpp("$builddir/tarlz") {
        progversion = "$tarlz_version",
        tarlz_sources,
        lzlib_sources,
        implicit_in = lzlib_implicit_sources,
    }
}

targets : foreach(function(target)
    acc(cross[target.name]) {
        target.os == "windows" and {}
        or build.zigcpp[target.name]("$builddir"/target.name/"tarlz"..target.exe) {
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
    "list.cc",
    "lunzcrash.cc",
    "lzip_index.cc",
    "main.cc",
    "md5.cc",
    "merge.cc",
    "mtester.cc",
    "nrep_stats.cc",
    "range_dec.cc",
    "reproduce.cc",
    "split.cc",
})

build{lziprecover_sources, lziprecover_implicit_sources} { "extract", url="$lziprecover_url", fmt="lzip", order_only_deps=lzip }

acc(host) {
    build.cpp("$builddir/lziprecover") {
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
        or build.zigcpp[target.name]("$builddir"/target.name/"lziprecover") {
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
    return build("$builddir/${release}-"..target.name..".tar.gz") { "tar",
        cross[target.name],
        prefix = "$builddir"/target.name,
    }
end)

phony "all" { archives }
help "all" "Build Lzip archives for Linux, MacOS and Windows"
