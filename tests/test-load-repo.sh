#!/bin/bash
set -euo pipefail

REPOBASEURL=https://ftp.icm.edu.pl/pub/Linux/fedora/linux/releases/30/
FUS_EXE=$G_TEST_BUILDDIR/fus

test_tmpdir=$(mktemp -d -t fus.test.XXXXXX)
trap "rm -rf $test_tmpdir" EXIT
err_file=$test_tmpdir/fus_error.log
out_file=$test_tmpdir/fus_out.log

test_remote_repo() {
    local modrepourl=$REPOBASEURL/Modular/x86_64/os/
    local pkgrepourl=$REPOBASEURL/Everything/x86_64/os/

    $FUS_EXE \
        -r modular,repo,$modrepourl \
        -r pkgs,repo,$pkgrepourl \
        -a x86_64 \
        libmodulemd 2>$err_file >$out_file

    if [ -n "$(cat $err_file)" ] || [ -z "$(cat $out_file)" ]; then
        echo "Error when running fus: $(cat $err_file)"
        exit 1
    fi
}

# Copied from the rpm-ostree project test suite
# builds a new RPM and adds it to the testdir's repo
# $1 - name
# $2+ - optional, treated as directive/value pairs
build_rpm() {
    local name=$1; shift
    local epoch=""
    local version=1.0
    local release=1
    local arch=x86_64

    mkdir -p $test_tmpdir/yumrepo/{specs,packages}
    local spec=$test_tmpdir/yumrepo/specs/$name.spec

    # write out the header
    cat > $spec << EOF
Name: $name
Summary: %{name}
License: GPLv2+
EOF

    local build= install= files= pretrans= pre= post= posttrans= post_args=
    local verifyscript= uinfo=
    local transfiletriggerin= transfiletriggerin_patterns=
    local transfiletriggerin2= transfiletriggerin2_patterns=
    local transfiletriggerun= transfiletriggerun_patterns=
    while [ $# -ne 0 ]; do
        local section=$1; shift
        local arg=$1; shift
        case $section in
        requires)
            echo "Requires: $arg" >> $spec;;
        provides)
            echo "Provides: $arg" >> $spec;;
        conflicts)
            echo "Conflicts: $arg" >> $spec;;
        post_args)
            post_args="$arg";;
        version|release|epoch|arch|build|install|files|pretrans|pre|post|posttrans|verifyscript|uinfo)
            declare $section="$arg";;
        transfiletriggerin)
            transfiletriggerin_patterns="$arg";
            declare $section="$1"; shift;;
        transfiletriggerin2)
            transfiletriggerin2_patterns="$arg";
            declare $section="$1"; shift;;
        transfiletriggerun)
            transfiletriggerun_patterns="$arg";
            declare $section="$1", shift;;
        *)
            assert_no_reached "unhandled section $section";;
        esac
    done

    cat >> $spec << EOF
Version: $version
Release: $release
${epoch:+Epoch: $epoch}
BuildArch: $arch

%description
%{summary}

# by default, we create a /usr/bin/$name script which just outputs $name
%build
echo -e "#!/bin/sh\necho $name-$version-$release.$arch" > $name
chmod a+x $name
$build

${pretrans:+%pretrans}
$pretrans

${pre:+%pre}
$pre

${post:+%post} ${post_args}
$post

${posttrans:+%posttrans}
$posttrans

${transfiletriggerin:+%transfiletriggerin -- ${transfiletriggerin_patterns}}
$transfiletriggerin

${transfiletriggerin2:+%transfiletriggerin2 -- ${transfiletriggerin2_patterns}}
$transfiletriggerin2

${transfiletriggerun:+%transfiletriggerun -- ${transfiletriggerun_patterns}}
$transfiletriggerun

${verifyscript:+%verifyscript}
$verifyscript

%install
mkdir -p %{buildroot}/usr/bin
install $name %{buildroot}/usr/bin
$install

%clean
rm -rf %{buildroot}

%files
/usr/bin/$name
$files
EOF

    local buildarch=$arch
    if [ "$arch" == "noarch" ]; then
        buildarch=$(uname -m)
    fi

    (cd $test_tmpdir/yumrepo/specs &&
     setarch $buildarch rpmbuild --target $arch -ba $name.spec \
        --define "_topdir $PWD" \
        --define "_sourcedir $PWD" \
        --define "_specdir $PWD" \
        --define "_builddir $PWD/.build" \
        --define "_srcrpmdir $PWD" \
        --define "_rpmdir $test_tmpdir/yumrepo/packages" \
        --define "_buildrootdir $PWD")
    # use --keep-all-metadata to retain previous updateinfo
    (cd $test_tmpdir/yumrepo &&
     createrepo_c --no-database --update --keep-all-metadata .)
    # convenience function to avoid follow-up add-pkg
    if [ -n "$uinfo" ]; then
        uinfo_cmd add-pkg $uinfo $name 0 $version $release $arch
    fi
    if test '!' -f $test_tmpdir/yumrepo.repo; then
        cat > $test_tmpdir/yumrepo.repo.tmp << EOF
[test-repo]
name=test-repo
baseurl=file:///$PWD/yumrepo
EOF

        mv $test_tmpdir/yumrepo.repo{.tmp,}
    fi
}

test_local_repo() {
    local pkgrepo="$test_tmpdir/yumrepo/"

    build_rpm sh provides "/usr/bin/sh"
    build_rpm foo
    build_rpm bar requires "foo = 1.0-1"

    $FUS_EXE \
        -r pkgs,repo,$pkgrepo \
        -a x86_64 \
        bar 2>$err_file >$out_file

    if [ -n "$(cat $err_file)" ] || [ -z "$(cat $out_file)" ]; then
        echo "Error when running fus: $(cat $err_file)"
        exit 1
    fi
}

test_remote_repo
test_local_repo
