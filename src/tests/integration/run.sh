#!/bin/bash

# To run darktable-cli must be found, either
#
#   1. Put darktable-cli in the PATH
#   2. Set DARKTABLE_CLI to the full pathname of darktable-cli executable
#
# To run the test suite:
#
#   ./run.sh               - will run all tests
#   ./run.sh 0001-exposure - will run the single tests 0001-exposure
#
# Options:
#
#   --disable-opencl           - do not run the OpenCL path
#   --no-deltae                - do a light check not requiring Delta-E module
#   --fast-fail                - abort testing on the first NOK test
#   --op=<n> | --operation=<n> - run test with matching operation n

CDPATH=

CLI=${DARKTABLE_CLI:-darktable-cli}
TEST_IMAGES=$PWD/images

TESTS=""
TEST_COUNT=0
TEST_ERROR=0
COMPARE=$(which compare)
DO_OPENCL=yes
DO_DELTAE=yes
DO_FAST_FAIL=no

[ -z $(which $CLI) ] && echo Make sure $CLI is in the path && exit 1

set -- $(getopt -q -u -o : -l disable-opencl,no-deltae,fast-fail,op:,operation: -- $*)

while [ $# -gt 0 ]; do
    case $1 in
        --disable-opencl)
            DO_OPENCL=no
            ;;
        --no-deltae)
            DO_DELTAE=no
            ;;
        --fast-fail)
            DO_FAST_FAIL=yes
            ;;
        --op|--operation)
            shift
            OP=$1
            TESTS=$(grep -l "operation=\"$OP\"" */*.xmp | while read xmp; do echo $(dirname $xmp); done)
            [ -z "$TESTS" ] && echo error: operation $OP did not macth any test && exit 1
            ;;
        (--)
            ;;
        (*)
            TESTS="$TESTS $(basename $1)"
            ;;
        (-*)
            echo "$0: error - unrecognized option $1"
            exit 1
            ;;
    esac
    shift
done

[ -z "$TESTS" ] && TESTS="$(ls -d [0-9]*)"

for dir in $TESTS; do
    echo Test $dir
    TEST_COUNT=$((TEST_COUNT + 1))

    if [ -f $dir/test.sh ]; then
        # The test has a specific driver
        (
            $dir/test.sh
        )

        if [ $? = 0 ]; then
            echo "  OK"
        else
            echo "  FAILS: specific test"
            TEST_ERROR=$((TEST_ERROR + 1))
        fi

    else
        # A standard test
        #   - xmp to create the output
        #   - expected. is the expected output
        #   - a diff is made to compute the max Delta-E
        (
            cd $dir

            # remove leading "????-"

            TEST=${dir:5}

            [ ! -f $TEST.xmp ] &&
                echo missing $dir.xmp && exit 1

            [ ! -f expected.png ] && echo "      missing expected.png"

            IMAGE=$(grep DerivedFrom $TEST.xmp | cut -d'"' -f2)

            echo "      Image $IMAGE"

            # Remove previous output and diff if any

            rm -f output*.png diff*.png

            # Create the output
            #
            # Note that we force host_memory_limit has this will have
            # impact on the tiling and will change the output.
            #
            # This means that the tiling algorithm is probably broken.
            #

            # All common core options:
            CORE_OPTIONS="--conf host_memory_limit=8192 \
                 --conf worker_threads=4 -t 4 \
                 --conf plugins/lighttable/export/force_lcms2=FALSE \
                 --conf plugins/lighttable/export/iccintent=0"

            # Some // loops seems to not honor the omp_set_num_threads() in
            # darktable.c (this is needed to run 0068-rawdenoise-xtrans on
            # different configurations)

            export OMP_THREAD_LIMIT=4

            $CLI --width 2048 --height 2048 \
                 --hq true --apply-custom-presets false \
                 "$TEST_IMAGES/$IMAGE" "$TEST.xmp" output.png \
                 --core --disable-opencl $CORE_OPTIONS 1> /dev/null  2> /dev/null

            res=$?

            if [ $DO_OPENCL == yes ]; then
                $CLI --width 2048 --height 2048 \
                     --hq true --apply-custom-presets false \
                     "$TEST_IMAGES/$IMAGE" "$TEST.xmp" output-cl.png \
                     --core $CORE_OPTIONS 1> /dev/null 2> /dev/null

                res=$((res + $?))
            fi

            # If all ok, check Delta-E

            if [ $res -eq 0 ]; then
                if [ ! -z "$COMPARE" -a $DO_OPENCL == yes ]; then
                    diffcount="$($COMPARE output.png output-cl.png -metric ae diff-cl.png 2>&1 )"

                    if [ $? -ne 0 ]; then
                        echo "      CPU & GPU version differ by ${diffcount} pixels"
                    fi
                fi

                if [ $DO_DELTAE == yes ]; then
                    if [ -f expected.png ]; then
                        ../deltae expected.png output.png
                    else
                        false
                    fi

                    res=$?

                    if [ $res -lt 2 ]; then
                        echo "  OK"
                        if [ $res = 1 ]; then
                            diffcount="$($COMPARE expected.png output.png -metric ae diff-ok.png 2>&1 )"
                        fi
                        res=0

                    else
                        echo "  FAILS: image visually changed"
                        if [ ! -z $COMPARE -a -f expected.png ]; then
                            diffcount="$($COMPARE expected.png output.png -metric ae diff.png 2>&1 )"
                            echo "         see diff.png for visual difference"
			    echo "         (${diffcount} pixels changed)"
                        fi
                    fi
                else
                    if [ -z $COMPARE ]; then
                        echo "no delta-e mode : required compare tool not found."
                        res=1
                    else
                        diffcount="$($COMPARE expected.png output.png -metric ae diff-ok.png 2>&1 )"

                        # if we have an exponent just pretend this is a number
                        # above 2000 which is the limit checked below.

                        if [[ $diffcount =~ e ]]; then
                            diffcount=50000
                        fi

                        if [[ $diffcount -lt 2000 ]]; then
                            echo "      Light check : OK"
                            res=0
                        else
                            echo "      Light check : NOK"
                            res=1
                        fi
                    fi
                fi
            else
                echo "  FAILS : darktable-cli errored"
                res=1
            fi

            if [ ! -f expected.png ]; then
                echo "  copy output.png to expected.png"
                echo "  optimize size of expected.png"

                if [ -z $(which zopflipng) ]; then
                    echo
                    echo "  ERROR: please install zopflipng tool."
                    exit 1
                fi

                zopflipng output.png expected.png 1> /dev/null 2>&1

                echo "  check that expected.png is correct:"
                echo "  \$ eog $(basename $PWD)/expected.png"
            fi

            exit $res
        )

        if [ $? -ne 0 ]; then
            TEST_ERROR=$((TEST_ERROR + 1))

            [ $DO_FAST_FAIL == yes ] && break;
        fi
    fi

    echo
done

echo
echo "Total test $TEST_COUNT"
echo "Errors     $TEST_ERROR"
