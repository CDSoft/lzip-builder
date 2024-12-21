# Lzip builder

This repository just bundles [Lzip](https://www.nongnu.org/lzip/) into a Git project.
It provides a [bang](https://github.com/CDSoft/bang) file to cross compile lzip for Linux, MacOS and Windows.

# Usage

```
$ bang
```

This generates the Ninja file for your platform (the provided Ninja file was generated for Linux).

```
$ ninja help
Distribution of lzip binaries for Linux, MacOS and Windows.

This Ninja build file will compile and install:

- lzip          (Linux, MacOS and Windows)
- plzip         (Linux and MacOS)
- tarlz         (Linux and MacOS)
- lziprecovery  (Linux and MacOS)

Targets:
  help      show this help message
  compile   Build Lzip binaries for the host only
  all       Build Lzip archives for Linux, MacOS and Windows
  install   install lzip in PREFIX or ~/.local
  clean     clean generated files
```

# Installation of Lzip

To install Lzip in `~/.local/bin`:

```
$ ninja install
```

Or in a custom directory (e.g. `/path/bin`):

```
$ PREFIX=/path/ ninja install
```

# Cross compilation of Lzip

You'll need [Zig](https://ziglang.org).

```
$ ninja all
```

Archives for all supported platforms are created in `.build`.
