#! /bin/bash

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
    $COLOR
    $CORRECT
    $TONE
    $EFFECT
)

###
# move module from one group to another
###

group_basic=(
    'base curve'
    'crop and rotate'
    'demosaic'
    'exposure'
    'graduated density'
    'input color profile'
    'invert'
    'lens correction'
    'orientation'
    'output color profile'
    'perspective correction'
    'raw black/white point'
    'rotate pixels'
    'scale pixels'
    'tone mapping'
    'unbreak input profile'
    'white balance'
)

group_tone=(
    'bloom'
    'color balance'
    'contrast brightness saturation'
    'equalizer'
    'fill light'
    'global tonemap'
    'levels'
    'local contrast'
    'shadows and highlights'
    'tone curve'
    'zone system'
    'filmic'
)

group_color=(
    'channel mixer'
    'color contrast'
    'color correction'
    'color look up table'
    'color mapping'
    'color transfer'
    'color zones'
    'colorize'
    'lowlight vision'
    'monochrome'
    'split toning'
    'velvia'
    'vibrance'
)

group_correct=(
    'chromatic aberrations'
    'color reconstruction'
    'defringe'
    'denoise (bilateral filter)'
    'denoise (non-local means)'
    'denoise (profiled)'
    'dithering'
    'haze removal'
    'highlight reconstruction'
    'hot pixels'
    'raw denoise'
)

group_effect=(
    'framing'
    'grain'
    'highpass'
    'liquify'
    'lowpass'
    'retouch'
    'sharpen'
    'soften'
    'spot removal'
    'vignetting'
    'watermark'
)

######################################### END OF CONFIGURATION HERE

FILE=$HOME/.config/darktable/darktablerc

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
        echo "plugins/darkroom/group/$name=$GROUP_POS" >> $FILE
    done
}

sed -i "/plugins\/darkroom\/group\//d" $FILE

set_iop_group $BASIC   "${group_basic[@]}"
set_iop_group $TONE    "${group_tone[@]}"
set_iop_group $COLOR   "${group_color[@]}"
set_iop_group $CORRECT "${group_correct[@]}"
set_iop_group $EFFECT  "${group_effect[@]}"
