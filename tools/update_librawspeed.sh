#!/bin/bash
# 

if [ ! -d "src/external" ];then
	echo please change to darktable root directory and run this script again
	exit
fi

repotmp=tmp
[ -d $repotmp ] || mkdir $repotmp
pushd $repotmp
git clone https://github.com/LibRaw/LibRaw.git
pushd LibRaw
git pull
popd
popd

for i in dcraw internal libraw src; do
	rsync -HaxvP $repotmp/LibRaw/$i/ src/external/LibRaw/$i/
done

pushd src/external/rawspeed
bash <(grep -v "git diff" update_rawspeed.sh)
popd


sed -ri  's/^\s+raw->params.document_mode/\/\/&/' src/common/imageio.c


rm -rf build;./build.sh
echo you may delete $repotmp or re-run this script

git status
