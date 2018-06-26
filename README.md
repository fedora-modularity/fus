## Known limitations

- Shared RPM artifacts are not supported


## Building

```
$ meson builddir
$ ninja -C builddir
```


## Usage

```
$ ./builddir/main --arch ARCH --repo NAME,TYPE,PATH --debug ITEM
```

There can be multiple repos. The name is just the name of the repository; type
can be `lookaside` or anything else.

Item to include in the input can have one of the following forms.

* `module(NAME)` or `module(NAME:STREAM)` for modules
* `group:foo` or `category:bar` for comps input
* just package name, or a glob matched against package names
