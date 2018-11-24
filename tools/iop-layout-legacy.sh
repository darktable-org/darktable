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
    $TONE
    $COLOR
    $CORRECT
    $EFFECT
)

###
# move module from one group to another
###

group_basic=(
    'base curve'
    'white balance'
    'shadows and highlights'
    'orientation'
    'raw black/white point'
    'color reconstruction'
    'demosaic'
    'contrast brightness saturation'
    'crop and rotate'
    'exposure'
    'highlight reconstruction'
    'invert'
)

group_tone=(
    'zone system'
    'tone curve'
    'tone mapping'
    'levels'
    'fill light'
    'local contrast'
    'global tonemap'
    'filmic'
)

group_color=(
    'unbreak input profile'
    'velvia'
    'vibrance'
    'color balance'
    'color contrast'
    'color correction'
    'color look up table'
    'output color profile'
    'channel mixer'
    'color transfer'
    'color zones'
    'input color profile'
    'monochrome'
)

group_correct=(
    'perspective correction'
    'raw denoise'
    'retouch'
    'rotate pixels'
    'scale pixels'
    'sharpen'
    'spot removal'
    'hot pixels'
    'defringe'
    'chromatic aberrations'
    'denoise (bilateral filter)'
    'denoise (non-local means)'
    'denoise (profiled)'
    'dithering'
    'equalizer'
    'lens correction'
    'liquify'
    'haze removal'
)

group_effect=(
    'watermark'
    'bloom'
    'vignetting'
    'split toning'
    'lowlight vision'
    'lowpass'
    'color mapping'
    'colorize'
    'framing'
    'graduated density'
    'grain'
    'highpass'
    'soften'
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
