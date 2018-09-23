## Purpose

This tool attempts to produce a viable "depsolved" collection of packagages.

Unlike earlier tools of this type, it understands the concept of modules and module
streams and can incorporate them into the dependency solving algorithm.

It takes as input a list of repositories and a list of artifacts of various types
that must be in the output package set.  The full list of input types is in "Usage"
below, but it includes individual RPMs, comps style groups and categories, and, as
mentioned above, modules, either whole or as part of a stream.

It then attempts to output a package set that includes all required artifacts as well
as all additional packages needed to allow the required artifacts to be installed.

NOTE: This does not mean that the entire output package set can be installed at once.
Many module use cases prevent this, as do some existing basic RPM use cases.

What it does mean is that for each output artifact, there is the potential to resolve
the RPM and module-level dependencies to allow it to install.


## Known limitations

- Shared RPM artifacts are not supported


## Building

```
$ meson builddir
$ ninja -C builddir
```


## Usage

```
$ ./builddir/main --repo NAME,TYPE,PATH --debug ITEM
```

There can be multiple repos. The name is just the name of the repository; type
can be `lookaside` or anything else.

Item to include in the input can have one of the following forms.

* `module(NAME)` or `module(NAME:STREAM)` for modules
* `group:foo` or `category:bar` for comps input
* just package name, or a glob matched against package names


## Testing

To run all the available tests:

```
$ ninja -C builddir test
```

Or to have more control on which tests are run, use `gtester`:

```
$ G_TEST_SRCDIR=$PWD/tests gtester builddir/tests
```
