#!/bin/bash

CDPATH=

PATTERN="[0-9]*"

[ ! -z $1 ] && PATTERN="$(basename $1)"

CLI=darktable-cli
TEST_IMAGES=$PWD/images

TEST_COUNT=0
TEST_ERROR=0
COMPARE=$(which compare)

[ -z $(which $CLI) ] && echo Make sure $CLI is in the path && exit 1

for dir in $(ls -d $PATTERN); do
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

            $CLI --width 2048 --height 2048 \
                 --hq true --apply-custom-presets false \
                 "$TEST_IMAGES/$IMAGE" "$TEST.xmp" output.png \
                 --core --disable-opencl $CORE_OPTIONS 1> /dev/null  2> /dev/null

            res=$?

            $CLI --width 2048 --height 2048 \
                 --hq true --apply-custom-presets false \
                 "$TEST_IMAGES/$IMAGE" "$TEST.xmp" output-cl.png \
                 --core $CORE_OPTIONS 1> /dev/null 2> /dev/null

            res=$((res + $?))

            # If all ok, check Delta-E

            if [ $res -eq 0 ]; then
                if [ ! -z $COMPARE ]; then
                    diffcount="$(compare output.png output-cl.png -metric ae diff-cl.png 2>&1 )"

                    if [ $? -ne 0 ]; then
                        echo "      CPU & GPU version differ by ${diffcount} pixels"
                    fi
                fi

                if [ -f expected.png ]; then
                    ../deltae expected.png output.png
                else
                    false
                fi

                res=$?

                if [ $res -lt 2 ]; then
                    echo "  OK"
                    if [ $res = 1 ]; then
                        diffcount="$(compare expected.png output.png -metric ae diff-ok.png 2>&1 )"
                    fi
                    res=0

                else
                    echo "  FAILS: image visually changed"
                    if [ ! -z $COMPARE -a -f expected.png ]; then
                        diffcount="$(compare expected.png output.png -metric ae diff.png 2>&1 )"
                        echo "         see diff.jpg for visual difference"
			echo "         (${diffcount} pixels changed)"
                    fi
                fi

            else
                echo "  FAILS : darktable-cli errored"
            fi

            if [ ! -f expected.png ]; then
                echo "  copy output.png to expected.png"
                echo "  check that expected.png is correct:"
                echo "  \$ eog $(basename $PWD)/expected.png"
                cp output.png expected.png
            fi

            exit $res
        )

        if [ $? -ne 0 ]; then
            TEST_ERROR=$((TEST_ERROR + 1))
        fi
    fi

    echo
done

echo
echo "Total test $TEST_COUNT"
echo "Errors     $TEST_ERROR"
