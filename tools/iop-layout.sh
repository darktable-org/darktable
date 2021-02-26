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
    $CORRECT
    $TONE
    $COLOR
    $EFFECT
)

###
# move module from one group to another
###

group_basic=(
    'basecurve'
    'clipping'
    'demosaic'
    'exposure'
    'graduatednd'
    'colorin'
    'invert'
    'lens'
    'flip'
    'colorout'
    'ashift'
    'rawprepare'
    'rotatepixels'
    'scalepixels'
    'tonemap'
    'profile_gamma'
    'temperature'
    'filmic'
    'filmicrgb'
    'basicadj'
    'negadoctor'
    'toneequal'
)

group_tone=(
    'bloom'
    'colisa'
    'atrous'
    'relight'
    'globaltonemap'
    'levels'
    'rgblevels'
    'bilat'
    'shadhi'
    'tonecurve'
    'zonesystem'
    'rgbcurve'
)

group_color=(
    'channelmixer'
    'colorbalance'
    'colorcontrast'
    'colorcorrection'
    'colorchecker'
    'colormapping'
    'colortransfer'
    'colorzones'
    'colorize'
    'lowlight'
    'lut3d'
    'monochrome'
    'splittoning'
    'velvia'
    'vibrance'
)

group_correct=(
    'cacorrect'
    'colorreconstruct'
    'defringe'
    'bilateral'
    'nlmeans'
    'denoiseprofile'
    'dither'
    'hazeremoval'
    'highlights'
    'hotpixels'
    'rawdenoise'
)

group_effect=(
    'borders'
    'grain'
    'highpass'
    'liquify'
    'lowpass'
    'retouch'
    'sharpen'
    'soften'
    'spots'
    'vignette'
    'watermark'
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
