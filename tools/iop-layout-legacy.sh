#!/bin/bash

if pgrep -x "darktable" > /dev/null ; then
    echo "error: darktable is running, please exit first"
    exit 1
fi

# do not touch the following 5 definitions

BASIC=1
TONE=2
COLOR=3
CORRECT=4
EFFECT=5

###
# module-group order, just reorder the module-group
###

module_group=(
    $BASIC
    $TONE
    $COLOR
    $CORRECT
    $EFFECT
)

###
# move module from one group to another
###

group_basic=(
    'basecurve'
    'temperature'
    'shadhi'
    'flip'
    'rawprepare'
    'colorreconstruct'
    'demosaic'
    'colisa'
    'clipping'
    'exposure'
    'highlights'
    'invert'
    'basicadj'
    'negadoctor'
    'toneequal'
)

group_tone=(
    'zonesystem'
    'tonecurve'
    'rgbcurve'
    'tonemapping'
    'levels'
    'rgblevels'
    'relight'
    'bilat'
    'globaltonemap'
    'filmic'
    'filmicrgb'
)

group_color=(
    'profile_gamma'
    'velvia'
    'vibrance'
    'colorbalance'
    'colorcontrast'
    'colorcorrection'
    'colorchecker'
    'colorout'
    'channelmixer'
    'colortransfer'
    'colorzones'
    'colorin'
    'monochrome'
)

group_correct=(
    'ashift'
    'rawdenoise'
    'retouch'
    'rotatepixels'
    'scalepixels'
    'sharpen'
    'spotremoval'
    'hotpixels'
    'defringe'
    'cacorrect'
    'bilateral'
    'nlmeans'
    'denoiseprofile'
    'dither'
    'atrous'
    'lens'
    'liquify'
    'hazeremoval'
)

group_effect=(
    'watermark'
    'bloom'
    'vignette'
    'splittoning'
    'lowlight'
    'lowpass'
    'colormapping'
    'colorize'
    'borders'
    'graduatednd'
    'grain'
    'highpass'
    'soften'
)

######################################### END OF CONFIGURATION HERE

[ -z $DT_CONFIGDIR ] && DT_CONFIGDIR=$HOME/.config/darktable

FILE=$DT_CONFIGDIR/darktablerc

[ ! -f $FILE ] && echo darktable configuration file 'darktablerc' does not exists && exit 1

BCK="$FILE.iop-conf-backup-$(date +%Y%m%d-%H%M%S)"

cp $FILE $BCK

echo backup will be created in:
echo $BCK
echo Do you want to continue?

select yn in "Yes" "No"; do
    case $yn in
        Yes ) break;;
        No ) exit;;
    esac
done

sed -i "/plugins\/darkroom\/group_order\//d" $FILE

pos=0
while [ "x${module_group[pos]}" != "x" ]; do
    group=${module_group[pos]}
    pos=$(( $pos + 1 ))
    echo "plugins/darkroom/group_order/$group=$pos" >> $FILE
done

function get_group_pos()
{
    local GROUP=$1

    pos=0
    while [ "x${module_group[pos]}" != "x" ]; do
        if [ ${module_group[pos]} == $GROUP ]; then
            echo $(( $pos + 1 ))
        fi
        pos=$(( $pos + 1 ));
    done
}

function set_iop_group()
{
    local GROUP_POS=$(get_group_pos $1)
    shift
    local LIST=("${@}")

    pos=0
    while [ "x${LIST[pos]}" != "x" ]; do
        name=${LIST[pos]}
        pos=$(( $pos + 1 ))
        echo "plugins/darkroom/$name/modulegroup=$GROUP_POS" >> $FILE
    done
}

sed -i "/plugins\/darkroom\/[^/]*\/modulegroup/d" $FILE

set_iop_group $BASIC   "${group_basic[@]}"
set_iop_group $TONE    "${group_tone[@]}"
set_iop_group $COLOR   "${group_color[@]}"
set_iop_group $CORRECT "${group_correct[@]}"
set_iop_group $EFFECT  "${group_effect[@]}"
