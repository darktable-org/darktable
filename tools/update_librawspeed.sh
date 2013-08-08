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

svn co https://rawstudio.org/svn/rawstudio/trunk/ rawstudio

popd


for i in dcraw internal libraw src; do
	rsync -HaxvP $repotmp/LibRaw/$i/ src/external/LibRaw/$i/
done

rsync -HaxvP --exclude .svn $repotmp/rawstudio/plugins/load-rawspeed/rawspeed/ src/external/rawspeed/RawSpeed/
rsync -HaxvP --exclude .svn $repotmp/rawstudio/plugins/load-rawspeed/data/ src/external/rawspeed/data/


sed -ri 's/printf\(\"/&[rawspeed] /' src/external/rawspeed/RawSpeed/RawDecoder.cpp
sed -ri  's/^\s+raw->params.document_mode/\/\/&/' src/common/imageio.c



git status


echo you may delete $repotmp or re-run this script
echo 
echo "add new files:"
echo pushd src/external/LibRaw/internal
echo git add aahd_demosaic.cpp dcb_demosaicing.c dht_demosaic.cpp wf_filtering.cpp
echo popd
echo
echo "for Pascal's changes:" 
echo "git checkout -- src/external/rawspeed/data/cameras.xml" 
echo ""
echo "rm -rf build;./build.sh"
